/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* GLSL to TGSI: the code generator, and the compiler's entry point.
 *
 * It walks the syntax tree once, checking types and emitting TGSI text as it
 * goes (glsl_gen.h has the value model). User functions are inlined at every
 * call; virglrenderer has no subroutines and the host's compiler cleans up
 * the copies. Control flow is break-only, because that is all the host
 * translation offers (BGNLOOP is `do { } while (true)`, there is no CONT and
 * no SWITCH): `continue` breaks out of a one-pass loop around the body,
 * `switch` is a one-pass loop of guarded case blocks, and a `return` from
 * inside a loop sets a flag that every loop end on the way out tests. */
#include "glsl_gen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

/* ---- arena and errors ------------------------------------------------------------- */

void *ar_alloc(struct glc *c, size_t n)
{
    n = (n + 15) & ~(size_t)15;
    struct arena_blk *b = c->arena;
    if (!b || b->used + n > b->cap) {
        size_t cap = n > 65536 ? n : 65536;
        b = malloc(sizeof *b + cap);
        if (!b) glc_error(c, "out of memory");
        b->next = c->arena; b->used = 0; b->cap = cap;
        c->arena = b;
    }
    void *p = b->data + b->used;
    b->used += n;
    memset(p, 0, n);
    return p;
}

char *ar_strndup(struct glc *c, const char *s, size_t n)
{
    char *r = ar_alloc(c, n + 1);
    memcpy(r, s, n);
    r[n] = 0;
    return r;
}

char *ar_strdup(struct glc *c, const char *s) { return ar_strndup(c, s, strlen(s)); }

void glc_error(struct glc *c, const char *fmt, ...)
{
    char tmp[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    snprintf(c->log, c->logsize, "0:%d: error: %s\n", c->line, tmp);
    longjmp(c->err, 1);
}

#define ERR(...) glc_error(g->c, __VA_ARGS__)

/* ---- types ---------------------------------------------------------------------------- */

struct gtype ty_scalar(enum gk k) { struct gtype t; memset(&t, 0, sizeof t); t.k = k; t.vec = k == GK_VOID ? 0 : 1; return t; }
struct gtype ty_vec(enum gk k, int n) { struct gtype t = ty_scalar(k); t.vec = (unsigned char)n; return t; }
struct gtype ty_mat(int cols, int rows) { struct gtype t = ty_scalar(GK_FLOAT); t.vec = (unsigned char)rows; t.cols = (unsigned char)cols; return t; }

int ty_eq(struct gtype a, struct gtype b)
{
    return a.k == b.k && a.vec == b.vec && a.cols == b.cols && a.array == b.array &&
           (a.k != GK_SAMPLER || a.sampler == b.sampler) && (a.k != GK_STRUCT || a.st == b.st);
}

int ty_is_matrix(struct gtype t) { return t.cols > 0 && !t.array; }
int ty_is_scalar(struct gtype t) { return !t.array && !t.cols && t.vec == 1 && t.k >= GK_FLOAT && t.k <= GK_BOOL; }
int ty_is_vector(struct gtype t) { return !t.array && !t.cols && t.k >= GK_FLOAT && t.k <= GK_BOOL; }
int ty_is_numeric(struct gtype t) { return !t.array && (t.k == GK_FLOAT || t.k == GK_INT || t.k == GK_UINT); }

struct gtype elem_type(struct gtype t) { t.array = 0; return t; }

int ty_elem_regs(struct gtype t)
{
    if (t.k == GK_STRUCT) {
        int n = 0;
        for (int i = 0; i < t.st->n; i++) n += ty_regs(t.st->f[i].t);
        return n;
    }
    if (t.cols) return t.cols;
    return 1;
}

int ty_regs(struct gtype t) { return ty_elem_regs(t) * (t.array > 0 ? t.array : 1); }

const char *ty_name(struct gtype t, char *buf, size_t n)
{
    const char *base;
    char tmp[32];
    if (t.k == GK_STRUCT) base = t.st->name;
    else if (t.k == GK_SAMPLER) base = zgl_type_name(t.sampler);
    else if (t.k == GK_VOID) base = "void";
    else if (t.cols) { if (t.cols == t.vec) snprintf(tmp, sizeof tmp, "mat%d", t.cols); else snprintf(tmp, sizeof tmp, "mat%dx%d", t.cols, t.vec); base = tmp; }
    else {
        static const char *const sc[] = { "", "float", "int", "uint", "bool" };
        static const char *const pre[] = { "", "vec", "ivec", "uvec", "bvec" };
        if (t.vec == 1) base = sc[t.k];
        else { snprintf(tmp, sizeof tmp, "%s%d", pre[t.k], t.vec); base = tmp; }
    }
    if (t.array) snprintf(buf, n, "%s[%d]", base, t.array);
    else snprintf(buf, n, "%s", base);
    return buf;
}

static enum zgl_type to_ztype(struct gtype t)
{
    if (t.k == GK_SAMPLER) return t.sampler;
    if (t.k == GK_STRUCT) return T_STRUCT;
    if (t.cols) {
        static const enum zgl_type m[5][5] = {
            [2] = { [2] = T_MAT2, [3] = T_MAT2X3, [4] = T_MAT2X4 },
            [3] = { [2] = T_MAT3X2, [3] = T_MAT3, [4] = T_MAT3X4 },
            [4] = { [2] = T_MAT4X2, [3] = T_MAT4X3, [4] = T_MAT4 },
        };
        return m[t.cols][t.vec];
    }
    static const enum zgl_type v[5][5] = {
        [GK_FLOAT] = { 0, T_FLOAT, T_VEC2, T_VEC3, T_VEC4 },
        [GK_INT] = { 0, T_INT, T_IVEC2, T_IVEC3, T_IVEC4 },
        [GK_UINT] = { 0, T_UINT, T_UVEC2, T_UVEC3, T_UVEC4 },
        [GK_BOOL] = { 0, T_BOOL, T_BVEC2, T_BVEC3, T_BVEC4 },
    };
    if (t.k >= GK_FLOAT && t.k <= GK_BOOL && t.vec >= 1 && t.vec <= 4) return v[t.k][t.vec];
    return T_VOID;
}

/* a built-in type name, or GK_VOID with *ok = 0 */
static struct gtype builtin_type(const char *s, int *ok)
{
    *ok = 1;
    if (!strcmp(s, "void")) return ty_scalar(GK_VOID);
    if (!strcmp(s, "float")) return ty_scalar(GK_FLOAT);
    if (!strcmp(s, "int")) return ty_scalar(GK_INT);
    if (!strcmp(s, "uint")) return ty_scalar(GK_UINT);
    if (!strcmp(s, "bool")) return ty_scalar(GK_BOOL);
    if (!strncmp(s, "vec", 3) && s[3] >= '2' && s[3] <= '4' && !s[4]) return ty_vec(GK_FLOAT, s[3] - '0');
    if (!strncmp(s, "ivec", 4) && s[4] >= '2' && s[4] <= '4' && !s[5]) return ty_vec(GK_INT, s[4] - '0');
    if (!strncmp(s, "uvec", 4) && s[4] >= '2' && s[4] <= '4' && !s[5]) return ty_vec(GK_UINT, s[4] - '0');
    if (!strncmp(s, "bvec", 4) && s[4] >= '2' && s[4] <= '4' && !s[5]) return ty_vec(GK_BOOL, s[4] - '0');
    if (!strncmp(s, "mat", 3) && s[3] >= '2' && s[3] <= '4') {
        if (!s[4]) return ty_mat(s[3] - '0', s[3] - '0');
        if (s[4] == 'x' && s[5] >= '2' && s[5] <= '4' && !s[6]) return ty_mat(s[3] - '0', s[5] - '0');
    }
    for (int t = 0; t < T_TYPE_COUNT; t++)
        if (zgl_type_is_sampler((enum zgl_type)t) && !strcmp(zgl_type_name((enum zgl_type)t), s)) {
            struct gtype r = ty_scalar(GK_SAMPLER);
            r.sampler = (enum zgl_type)t;
            return r;
        }
    if (!strcmp(s, "samplerExternalOES")) { struct gtype r = ty_scalar(GK_SAMPLER); r.sampler = T_SAMPLER2D; return r; }
    *ok = 0;
    return ty_scalar(GK_VOID);
}

static struct gstruct *find_struct(struct gen *g, const char *name)
{
    for (int i = g->nstructs - 1; i >= 0; i--) if (!strcmp(g->structs[i]->name, name)) return g->structs[i];
    return NULL;
}

static struct val expr(struct gen *g, struct node *n);
static int const_int(struct gen *g, struct node *n, const char *what);

static void declare_struct(struct gen *g, struct tspec *ts);

/* the type a type specifier names, with its array suffix (not the declarator's) */
static struct gtype resolve_tspec(struct gen *g, struct tspec *ts)
{
    struct gtype t;
    if (ts->nfields) declare_struct(g, ts);
    int ok;
    t = builtin_type(ts->name, &ok);
    if (!ok) {
        struct gstruct *s = find_struct(g, ts->name);
        if (!s) {
            if (!strcmp(ts->name, "double") || !strncmp(ts->name, "dvec", 4) || !strncmp(ts->name, "dmat", 4)) ERR("double precision is not supported");
            ERR("unknown type '%s'", ts->name);
        }
        t = ty_scalar(GK_STRUCT);
        t.st = s;
    }
    if (ts->has_array) {
        if (ts->array && ts->array->k != N_EMPTY) { t.array = const_int(g, ts->array, "array size"); if (t.array <= 0) ERR("array size must be positive"); }
        else t.array = -1;
    }
    return t;
}

static struct gtype with_array(struct gen *g, struct gtype t, struct node *decl)
{
    if (!decl->has_array) return t;
    if (t.array) ERR("arrays of arrays are not supported");
    if (decl->array && decl->array->k != N_EMPTY) { t.array = const_int(g, decl->array, "array size"); if (t.array <= 0) ERR("array size must be positive"); }
    else t.array = -1;
    return t;
}

static void declare_struct(struct gen *g, struct tspec *ts)
{
    for (int i = g->nstructs - 1; i >= 0; i--)
        if (g->structs[i]->name == ts->name) return;           /* this very definition, seen before */
    struct gstruct *s = ar_alloc(g->c, sizeof *s);
    s->name = ts->name;
    int n = 0;
    for (int i = 0; i < ts->nfields; i++) n += ts->fields[i]->n;
    s->f = ar_alloc(g->c, (size_t)(n ? n : 1) * sizeof *s->f);
    for (int i = 0; i < ts->nfields; i++) {
        struct node *d = ts->fields[i];
        struct gtype ft = resolve_tspec(g, d->ts);
        for (int k = 0; k < d->n; k++) {
            struct gtype t = with_array(g, ft, d->l[k]);
            if (t.array < 0) ERR("struct member '%s' needs an array size", d->l[k]->name);
            if (t.k == GK_SAMPLER) ERR("samplers in structs are not supported");
            for (int j = 0; j < s->n; j++) if (!strcmp(s->f[j].name, d->l[k]->name)) ERR("duplicate member '%s'", d->l[k]->name);
            s->f[s->n].name = d->l[k]->name;
            s->f[s->n].t = t;
            s->n++;
        }
    }
    if (g->nstructs == g->capstructs) {
        int cap = g->capstructs ? g->capstructs * 2 : 16;
        struct gstruct **v = ar_alloc(g->c, (size_t)cap * sizeof *v);
        if (g->nstructs) memcpy(v, g->structs, (size_t)g->nstructs * sizeof *v);
        g->structs = v; g->capstructs = cap;
    }
    g->structs[g->nstructs++] = s;
}

/* ---- output ------------------------------------------------------------------------------ */

static void out_put(struct gen *g, const char *s, size_t n)
{
    if (g->len + n + 1 > g->cap) {
        size_t cap = g->cap ? g->cap * 2 : 16384;
        while (cap < g->len + n + 1) cap *= 2;
        char *b = realloc(g->code, cap);
        if (!b) ERR("out of memory");
        g->code = b; g->cap = cap;
    }
    memcpy(g->code + g->len, s, n);
    g->len += n;
    g->code[g->len] = 0;
}

void emitf(struct gen *g, const char *fmt, ...)
{
    char line[512];
    int k = snprintf(line, sizeof line, "%4d: ", g->ninst++);
    va_list ap;
    va_start(ap, fmt);
    k += vsnprintf(line + k, sizeof line - (size_t)k, fmt, ap);
    va_end(ap);
    if (k > (int)sizeof line - 2) k = (int)sizeof line - 2;
    line[k++] = '\n';
    out_put(g, line, (size_t)k);
}

void emit(struct gen *g, const char *op, const char *dst, const char *s0, const char *s1, const char *s2, const char *s3)
{
    char line[480];
    int k = snprintf(line, sizeof line, "%s", op);
    if (dst) k += snprintf(line + k, sizeof line - (size_t)k, " %s", dst);
    const char *s[4] = { s0, s1, s2, s3 };
    for (int i = 0; i < 4; i++) if (s[i]) k += snprintf(line + k, sizeof line - (size_t)k, "%s %s", (dst || i) ? "," : "", s[i]);
    emitf(g, "%s", line);
}

static const char *file_name(enum file f)
{
    switch (f) {
    case F_TEMP: return "TEMP"; case F_IN: return "IN"; case F_OUT: return "OUT"; case F_CONST: return "CONST";
    case F_IMM: return "IMM"; case F_SV: return "SV"; case F_SAMP: return "SAMP"; default: return "?";
    }
}

static int ncomp(struct gtype t) { return t.vec ? t.vec : 1; }

void src_str(struct gen *g, char *buf, size_t n, struct val v, int reg)
{
    (void)g;
    char sw[5];
    int nc = ncomp(v.t);
    for (int i = 0; i < 4; i++) sw[i] = "xyzw"[v.swz[i < nc ? i : nc - 1] & 3];
    sw[4] = 0;
    char r[64];
    if (v.ind) {
        if (v.arr) snprintf(r, sizeof r, "%s[ADDR[0].x+%d](%d)", file_name(v.file), v.index + reg, v.arr);
        else snprintf(r, sizeof r, "%s[ADDR[0].x+%d]", file_name(v.file), v.index + reg);
    } else snprintf(r, sizeof r, "%s[%d]", file_name(v.file), v.index + reg);
    if (v.abs) snprintf(buf, n, "%s|%s.%s|", v.neg ? "-" : "", r, sw);
    else snprintf(buf, n, "%s%s.%s", v.neg ? "-" : "", r, sw);
}

void dst_str(struct gen *g, char *buf, size_t n, struct val v, int reg, int nc)
{
    (void)g;
    char m[5] = { 0 };
    int used[4] = { 0 };
    for (int i = 0; i < nc && i < 4; i++) used[v.swz[i] & 3] = 1;
    int k = 0;
    for (int i = 0; i < 4; i++) if (used[i]) m[k++] = "xyzw"[i];
    if (v.ind) {
        if (v.arr) snprintf(buf, n, "%s[ADDR[0].x+%d](%d).%s", file_name(v.file), v.index + reg, v.arr, m);
        else snprintf(buf, n, "%s[ADDR[0].x+%d].%s", file_name(v.file), v.index + reg, m);
    } else snprintf(buf, n, "%s[%d].%s", file_name(v.file), v.index + reg, m);
}

/* ---- values -------------------------------------------------------------------------------- */

static void ident_swz(struct val *v) { for (int i = 0; i < 4; i++) v->swz[i] = (unsigned char)i; }

struct val v_temp(struct gen *g, struct gtype t)
{
    struct val v;
    memset(&v, 0, sizeof v);
    v.t = t;
    v.file = F_TEMP;
    v.index = g->ntemps;
    int n = ty_regs(t);
    if (n < 1) n = 1;
    g->ntemps += n;
    if (n > 1 || t.array) {
        if (g->narrays >= 512) ERR("too many arrays");
        g->arrays[g->narrays].first = v.index;
        g->arrays[g->narrays].n = n;
        g->narrays++;
        v.arr = g->narrays;
    }
    ident_swz(&v);
    v.lv = 1;
    return v;
}

static char imm_kind(enum gk k) { return k == GK_FLOAT ? 'F' : k == GK_INT ? 'I' : 'U'; }

struct val v_imm(struct gen *g, struct gtype t, const uint32_t *comps)
{
    struct val v;
    memset(&v, 0, sizeof v);
    v.t = t;
    v.is_const = 1;
    int nc = ncomp(t);
    for (int i = 0; i < nc; i++) v.cv[i] = comps[i];
    uint32_t w[4];
    for (int i = 0; i < 4; i++) w[i] = comps[i < nc ? i : nc - 1];
    char k = imm_kind(t.k);
    int idx = -1;
    for (int i = 0; i < g->nimm; i++)
        if (g->imm[i].k == k && !memcmp(g->imm[i].v, w, sizeof w)) { idx = i; break; }
    if (idx < 0) {
        if (g->nimm >= 1024) ERR("too many constants");
        idx = g->nimm++;
        g->imm[idx].k = k;
        memcpy(g->imm[idx].v, w, sizeof w);
    }
    v.file = F_IMM;
    v.index = idx;
    ident_swz(&v);
    return v;
}

struct val v_fimm(struct gen *g, float f) { uint32_t u; memcpy(&u, &f, 4); return v_imm(g, ty_scalar(GK_FLOAT), &u); }
struct val v_iimm(struct gen *g, int i) { uint32_t u = (uint32_t)i; return v_imm(g, ty_scalar(GK_INT), &u); }
struct val v_uimm(struct gen *g, uint32_t u) { return v_imm(g, ty_scalar(GK_UINT), &u); }
static struct val v_bimm(struct gen *g, int b) { uint32_t u = b ? 0xFFFFFFFFu : 0; return v_imm(g, ty_scalar(GK_BOOL), &u); }

float cv_f(const struct val *v, int k) { float f; memcpy(&f, &v->cv[k], 4); return f; }
int32_t cv_i(const struct val *v, int k) { return (int32_t)v->cv[k]; }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* a constant matrix/vector value as an immediate-backed val (matrices: one IMM per column) */
static struct val const_val(struct gen *g, struct gtype t, const uint32_t *cv)
{
    if (!t.cols) return v_imm(g, t, cv);
    struct val r = v_temp(g, t);
    for (int col = 0; col < t.cols; col++) {
        struct val c = v_imm(g, ty_vec(GK_FLOAT, t.vec), cv + col * t.vec);
        char d[96], s[96];
        struct val rc = r; rc.t = ty_vec(GK_FLOAT, t.vec);
        dst_str(g, d, sizeof d, rc, col, t.vec);
        src_str(g, s, sizeof s, c, 0);
        emit(g, "MOV", d, s, NULL, NULL, NULL);
    }
    r.is_const = 1;
    memcpy(r.cv, cv, (size_t)(t.cols * t.vec) * 4);
    r.lv = 0;
    return r;
}

struct val v_comp(struct val v, int k)
{
    struct val r = v;
    r.t = ty_scalar(v.t.k);
    unsigned char c = v.swz[k];
    for (int i = 0; i < 4; i++) r.swz[i] = c;
    if (v.is_const) { r.cv[0] = v.cv[k]; }
    return r;
}

struct val v_col(struct gen *g, struct val m, int k)
{
    (void)g;
    struct val r = m;
    r.t = ty_vec(GK_FLOAT, m.t.vec);
    r.index = m.index + k;
    ident_swz(&r);
    if (m.is_const) memmove(r.cv, m.cv + k * m.t.vec, (size_t)m.t.vec * 4);
    return r;
}

struct val v_bcast(struct val v, int n)
{
    if (v.t.vec == 1 && !v.t.cols && !v.t.array && n > 1) {
        for (int i = 1; i < 4; i++) v.swz[i] = v.swz[0];
        for (int i = 1; i < n; i++) v.cv[i] = v.cv[0];
        v.t.vec = (unsigned char)n;
    }
    return v;
}

struct val v_swizzle(struct val v, const int *idx, int n)
{
    struct val r = v;
    r.t = ty_vec(v.t.k, n);
    uint32_t cv[4];
    int dup = 0;
    for (int i = 0; i < n; i++) {
        r.swz[i] = v.swz[idx[i]];
        cv[i] = v.cv[idx[i]];
        for (int j = 0; j < i; j++) if (idx[j] == idx[i]) dup = 1;
    }
    for (int i = n; i < 4; i++) r.swz[i] = r.swz[n - 1];
    if (v.is_const) memcpy(r.cv, cv, (size_t)n * 4);
    if (dup) r.lv = 0;
    return r;
}

/* indirect values become a temp copy: one UARL, then a MOV per register */
static struct val reg_val(enum file f, int index, struct gtype t, int lv);

/* an I/O array element picked at run time: compare and select, element by element */
static struct val io_select(struct gen *g, struct val v)
{
    struct val sel = reg_val(F_TEMP, v.ind_reg, ty_scalar(GK_INT), 0);
    struct val base = v; base.ind = 0;
    struct val r = base;                    /* element 0 */
    unsigned char swz[4]; memcpy(swz, v.swz, 4);
    for (int e = 1; e < v.ind_count; e++) {
        struct val el = base; el.index = base.index + e * v.ind_stride;
        struct val eq = op2(g, "USEQ", ty_scalar(GK_BOOL), sel, v_iimm(g, e));
        r = v_select(g, eq, el, r);
    }
    if (!v.t.array && v.t.k != GK_STRUCT && !v.t.cols) memcpy(r.swz, swz, 4);
    r.lv = 0;
    return r;
}

struct val v_direct(struct gen *g, struct val v)
{
    if (v.ind == 2) return io_select(g, v);
    if (!v.ind) return v;
    g->uses_addr = 1;
    char s[96], d[96];
    snprintf(s, sizeof s, "TEMP[%d].xxxx", v.ind_reg);
    emit(g, "UARL", "ADDR[0].x", s, NULL, NULL, NULL);
    struct gtype t = v.t;
    struct val r = v_temp(g, t);
    int n = ty_regs(t);
    for (int i = 0; i < n; i++) {
        struct val sv = v; ident_swz(&sv);
        struct val dv = r; ident_swz(&dv);
        sv.t = dv.t = ty_vec(GK_FLOAT, 4);
        src_str(g, s, sizeof s, sv, i);
        dst_str(g, d, sizeof d, dv, i, 4);
        emit(g, "MOV", d, s, NULL, NULL, NULL);
    }
    /* keep the component selection of a vector access */
    if (!t.array && t.k != GK_STRUCT && !t.cols) memcpy(r.swz, v.swz, 4);
    r.lv = 0;
    return r;
}

/* a fresh temp holding v */
struct val v_copy(struct gen *g, struct val v)
{
    v = v_direct(g, v);
    struct val r = v_temp(g, v.t);
    int n = ty_regs(v.t);
    if (n == 1 && !v.t.array && v.t.k != GK_STRUCT) {
        char d[96], s[96];
        dst_str(g, d, sizeof d, r, 0, ncomp(v.t));
        src_str(g, s, sizeof s, v, 0);
        emit(g, "MOV", d, s, NULL, NULL, NULL);
    } else {
        for (int i = 0; i < n; i++) {
            struct val sv = v, dv = r;
            ident_swz(&sv); ident_swz(&dv);
            sv.t = dv.t = ty_vec(GK_FLOAT, 4); sv.neg = sv.abs = 0;
            char d[96], s[96];
            dst_str(g, d, sizeof d, dv, i, 4);
            src_str(g, s, sizeof s, sv, i);
            emit(g, "MOV", d, s, NULL, NULL, NULL);
        }
    }
    if (v.is_const) { r.is_const = 1; memcpy(r.cv, v.cv, sizeof r.cv); }
    return r;
}

/* store src into the lvalue dst (same type, conversions done) */
void v_store(struct gen *g, struct val dst, struct val src)
{
    src = v_direct(g, src);
    char d[96], s[96];
    if (dst.ind == 2) {
        /* write the element the index names: a branch per element */
        src = v_copy(g, src);
        for (int e = 0; e < dst.ind_count; e++) {
            snprintf(s, sizeof s, "TEMP[%d].xxxx", dst.ind_reg);
            struct val eq = op2(g, "USEQ", ty_scalar(GK_BOOL), reg_val(F_TEMP, dst.ind_reg, ty_scalar(GK_INT), 0), v_iimm(g, e));
            src_str(g, s, sizeof s, eq, 0);
            emit(g, "UIF", NULL, s, NULL, NULL, NULL);
            struct val el = dst; el.ind = 0; el.index = dst.index + e * dst.ind_stride;
            v_store(g, el, src);
            emit(g, "ENDIF", NULL, NULL, NULL, NULL, NULL);
        }
        return;
    }
    if (dst.ind) {
        g->uses_addr = 1;
        snprintf(s, sizeof s, "TEMP[%d].xxxx", dst.ind_reg);
        emit(g, "UARL", "ADDR[0].x", s, NULL, NULL, NULL);
    }
    int n = ty_regs(dst.t);
    if (n == 1 && !dst.t.array && dst.t.k != GK_STRUCT && !dst.t.cols) {
        /* a vector: the source's i-th component goes to the destination's i-th component */
        int nc = ncomp(dst.t);
        struct val sv = v_bcast(src, nc);
        unsigned char sw[4];
        for (int i = 0; i < 4; i++) sw[i] = sv.swz[0];
        for (int i = 0; i < nc; i++) sw[dst.swz[i] & 3] = sv.swz[i < ncomp(sv.t) ? i : 0];
        struct val p = sv;
        p.t = ty_vec(sv.t.k, 4);
        memcpy(p.swz, sw, 4);
        dst_str(g, d, sizeof d, dst, 0, nc);
        src_str(g, s, sizeof s, p, 0);
        emit(g, "MOV", d, s, NULL, NULL, NULL);
        return;
    }
    for (int i = 0; i < n; i++) {
        struct val sv = src, dv = dst;
        ident_swz(&sv); ident_swz(&dv);
        sv.t = dv.t = ty_vec(GK_FLOAT, 4); sv.neg = sv.abs = 0;
        dst_str(g, d, sizeof d, dv, i, 4);
        src_str(g, s, sizeof s, sv, i);
        emit(g, "MOV", d, s, NULL, NULL, NULL);
    }
}

/* ---- operations ----------------------------------------------------------------------------- */

static struct val opn(struct gen *g, const char *op, struct gtype rt, struct val *a, int n)
{
    struct val r = v_temp(g, rt);
    char d[96], s[4][96];
    for (int i = 0; i < n; i++) { a[i] = v_direct(g, a[i]); src_str(g, s[i], sizeof s[i], a[i], 0); }
    dst_str(g, d, sizeof d, r, 0, ncomp(rt));
    emit(g, op, d, n > 0 ? s[0] : NULL, n > 1 ? s[1] : NULL, n > 2 ? s[2] : NULL, NULL);
    r.lv = 0;
    return r;
}

struct val op1(struct gen *g, const char *op, struct gtype rt, struct val a) { return opn(g, op, rt, &a, 1); }
struct val op2(struct gen *g, const char *op, struct gtype rt, struct val a, struct val b) { struct val v[2] = { a, b }; return opn(g, op, rt, v, 2); }
struct val op3(struct gen *g, const char *op, struct gtype rt, struct val a, struct val b, struct val c) { struct val v[3] = { a, b, c }; return opn(g, op, rt, v, 3); }

/* scalar-only opcodes (RCP, RSQ, EX2, LG2, SIN, COS, POW): one per component */
struct val scalar_op1(struct gen *g, const char *op, struct val a)
{
    int n = ncomp(a.t);
    if (n == 1) return op1(g, op, ty_scalar(GK_FLOAT), a);
    a = v_direct(g, a);
    struct val r = v_temp(g, ty_vec(GK_FLOAT, n));
    for (int i = 0; i < n; i++) {
        char d[96], s[96];
        struct val rc = v_comp(r, i), ac = v_comp(a, i);
        dst_str(g, d, sizeof d, rc, 0, 1);
        src_str(g, s, sizeof s, ac, 0);
        emit(g, op, d, s, NULL, NULL, NULL);
    }
    r.lv = 0;
    return r;
}

struct val scalar_op2(struct gen *g, const char *op, struct val a, struct val b)
{
    int n = ncomp(a.t) > ncomp(b.t) ? ncomp(a.t) : ncomp(b.t);
    a = v_bcast(v_direct(g, a), n); b = v_bcast(v_direct(g, b), n);
    if (n == 1) return op2(g, op, ty_scalar(GK_FLOAT), a, b);
    struct val r = v_temp(g, ty_vec(GK_FLOAT, n));
    for (int i = 0; i < n; i++) {
        char d[96], s1[96], s2[96];
        dst_str(g, d, sizeof d, v_comp(r, i), 0, 1);
        src_str(g, s1, sizeof s1, v_comp(a, i), 0);
        src_str(g, s2, sizeof s2, v_comp(b, i), 0);
        emit(g, op, d, s1, s2, NULL, NULL);
    }
    r.lv = 0;
    return r;
}

/* ---- constant folding --------------------------------------------------------------------------- */

static int both_const(struct val a, struct val b) { return a.is_const && b.is_const && !a.t.cols && !b.t.cols && !a.t.array && !b.t.array; }

static uint32_t fold1(enum gk k, const char *op, uint32_t x, uint32_t y, int *ok)
{
    *ok = 1;
    if (k == GK_FLOAT) {
        float a, b, r = 0;
        memcpy(&a, &x, 4); memcpy(&b, &y, 4);
        if (!strcmp(op, "+")) r = a + b;
        else if (!strcmp(op, "-")) r = a - b;
        else if (!strcmp(op, "*")) r = a * b;
        else if (!strcmp(op, "/")) r = a / b;
        else if (!strcmp(op, "<")) return a < b ? ~0u : 0;
        else if (!strcmp(op, ">")) return a > b ? ~0u : 0;
        else if (!strcmp(op, "<=")) return a <= b ? ~0u : 0;
        else if (!strcmp(op, ">=")) return a >= b ? ~0u : 0;
        else if (!strcmp(op, "==")) return a == b ? ~0u : 0;
        else if (!strcmp(op, "!=")) return a != b ? ~0u : 0;
        else { *ok = 0; return 0; }
        return f2u(r);
    }
    int32_t a = (int32_t)x, b = (int32_t)y;
    int u = k == GK_UINT || k == GK_BOOL;
    if (!strcmp(op, "+")) return x + y;
    if (!strcmp(op, "-")) return x - y;
    if (!strcmp(op, "*")) return x * y;
    if (!strcmp(op, "/")) { if (!y) return 0; return u ? x / y : (b == -1 ? (uint32_t)-a : (uint32_t)(a / b)); }
    if (!strcmp(op, "%")) { if (!y) return 0; return u ? x % y : (b == -1 ? 0 : (uint32_t)(a % b)); }
    if (!strcmp(op, "&") || !strcmp(op, "&&")) return x & y;
    if (!strcmp(op, "|") || !strcmp(op, "||")) return x | y;
    if (!strcmp(op, "^") || !strcmp(op, "^^")) return x ^ y;
    if (!strcmp(op, "<<")) return x << (y & 31);
    if (!strcmp(op, ">>")) return u ? x >> (y & 31) : (uint32_t)(a >> (y & 31));
    if (!strcmp(op, "<")) return (u ? x < y : a < b) ? ~0u : 0;
    if (!strcmp(op, ">")) return (u ? x > y : a > b) ? ~0u : 0;
    if (!strcmp(op, "<=")) return (u ? x <= y : a <= b) ? ~0u : 0;
    if (!strcmp(op, ">=")) return (u ? x >= y : a >= b) ? ~0u : 0;
    if (!strcmp(op, "==")) return x == y ? ~0u : 0;
    if (!strcmp(op, "!=")) return x != y ? ~0u : 0;
    *ok = 0;
    return 0;
}

static uint32_t conv1(enum gk from, enum gk to, uint32_t x)
{
    float f;
    if (from == to) return x;
    switch (to) {
    case GK_FLOAT:
        if (from == GK_INT) f = (float)(int32_t)x;
        else if (from == GK_UINT) f = (float)x;
        else f = x ? 1.0f : 0.0f;
        return f2u(f);
    case GK_INT:
    case GK_UINT:
        if (from == GK_FLOAT) { memcpy(&f, &x, 4); if (to == GK_INT) return (uint32_t)(int32_t)f; return f <= 0 ? 0 : (uint32_t)f; }
        if (from == GK_BOOL) return x ? 1 : 0;
        return x;
    case GK_BOOL:
        if (from == GK_FLOAT) { memcpy(&f, &x, 4); return f != 0 ? ~0u : 0; }
        return x ? ~0u : 0;
    default: return x;
    }
}

/* ---- conversions -------------------------------------------------------------------------------- */

static int implicit_ok(struct gtype from, struct gtype to)
{
    if (ty_eq(from, to)) return 1;
    if (from.array || to.array || from.vec != to.vec || from.cols != to.cols) return 0;
    if (to.k == GK_FLOAT && (from.k == GK_INT || from.k == GK_UINT)) return 1;
    if (to.k == GK_UINT && from.k == GK_INT) return 1;                     /* 4.00 rules, accepted everywhere */
    return 0;
}

struct val v_convert(struct gen *g, struct val v, enum gk to)
{
    if (v.t.k == to) return v;
    if (v.t.array || v.t.k == GK_STRUCT || v.t.k == GK_SAMPLER || v.t.k == GK_VOID) ERR("cannot convert this value");
    if (v.t.cols) {
        if (to != GK_FLOAT) ERR("matrices are float only");
        return v;
    }
    int n = ncomp(v.t);
    struct gtype rt = ty_vec(to, n);
    if (v.is_const) {
        uint32_t cv[4];
        for (int i = 0; i < n; i++) cv[i] = conv1(v.t.k, to, v.cv[i]);
        return v_imm(g, rt, cv);
    }
    enum gk from = v.t.k;
    if ((from == GK_INT && to == GK_UINT) || (from == GK_UINT && to == GK_INT)) { v.t = rt; return v; }
    switch (to) {
    case GK_FLOAT:
        if (from == GK_INT) return op1(g, "I2F", rt, v);
        if (from == GK_UINT) return op1(g, "U2F", rt, v);
        return op2(g, "AND", rt, v, v_bcast(v_uimm(g, 0x3F800000u), n));
    case GK_INT:
        if (from == GK_FLOAT) return op1(g, "F2I", rt, v);
        return op2(g, "AND", rt, v, v_bcast(v_uimm(g, 1), n));
    case GK_UINT:
        if (from == GK_FLOAT) return op1(g, "F2U", rt, v);
        return op2(g, "AND", rt, v, v_bcast(v_uimm(g, 1), n));
    case GK_BOOL:
        if (from == GK_FLOAT) return op2(g, "FSNE", rt, v, v_bcast(v_fimm(g, 0), n));
        return op2(g, "USNE", rt, v, v_bcast(v_uimm(g, 0), n));
    default: break;
    }
    ERR("bad conversion");
}

/* v converted (implicitly) to t, or an error naming `what` */
static struct val coerce(struct gen *g, struct val v, struct gtype t, const char *what)
{
    if (ty_eq(v.t, t)) return v;
    if (!implicit_ok(v.t, t)) {
        char a[48], b[48];
        ERR("%s: cannot convert %s to %s", what, ty_name(v.t, a, sizeof a), ty_name(t, b, sizeof b));
    }
    struct val r = v_convert(g, v, t.k);
    r.t = t;
    return r;
}

/* ---- arithmetic -------------------------------------------------------------------------------- */

static const char *opcode(struct gen *g, const char *op, enum gk k)
{
    int f = k == GK_FLOAT, i = k == GK_INT;
    if (!strcmp(op, "+")) return f ? "ADD" : "UADD";
    if (!strcmp(op, "*")) return f ? "MUL" : "UMUL";
    if (!strcmp(op, "/")) return f ? "DIV" : i ? "IDIV" : "UDIV";
    if (!strcmp(op, "%")) { if (f) ERR("'%%' needs integers"); return i ? "MOD" : "UMOD"; }
    if (!strcmp(op, "&")) return "AND";
    if (!strcmp(op, "|")) return "OR";
    if (!strcmp(op, "^")) return "XOR";
    if (!strcmp(op, "<<")) return "SHL";
    if (!strcmp(op, ">>")) return i ? "ISHR" : "USHR";
    if (!strcmp(op, "<")) return f ? "FSLT" : i ? "ISLT" : "USLT";
    if (!strcmp(op, ">=")) return f ? "FSGE" : i ? "ISGE" : "USGE";
    if (!strcmp(op, "==")) return f ? "FSEQ" : "USEQ";
    if (!strcmp(op, "!=")) return f ? "FSNE" : "USNE";
    return NULL;
}

struct val v_not(struct gen *g, struct val b)
{
    if (b.is_const) { uint32_t cv[4]; for (int i = 0; i < ncomp(b.t); i++) cv[i] = ~b.cv[i]; return v_imm(g, b.t, cv); }
    return op1(g, "NOT", b.t, b);
}

struct val v_select(struct gen *g, struct val cond, struct val a, struct val b)
{
    if (cond.is_const && ncomp(cond.t) == 1) return cond.cv[0] ? a : b;
    if (ty_regs(a.t) == 1 && !a.t.array && a.t.k != GK_STRUCT && !a.t.cols)
        return op3(g, "UCMP", a.t, v_bcast(cond, ncomp(a.t)), a, b);
    /* aggregates: a register at a time, with the scalar condition */
    a = v_direct(g, a); b = v_direct(g, b);
    struct val r = v_temp(g, a.t);
    struct val cs = v_bcast(v_comp(cond, 0), 4);
    for (int i = 0; i < ty_regs(a.t); i++) {
        struct val av = a, bv = b, rv = r;
        ident_swz(&av); ident_swz(&bv); ident_swz(&rv);
        av.t = bv.t = rv.t = ty_vec(GK_FLOAT, 4);
        char d[96], s0[96], s1[96], s2[96];
        dst_str(g, d, sizeof d, rv, i, 4);
        src_str(g, s0, sizeof s0, cs, 0);
        src_str(g, s1, sizeof s1, av, i);
        src_str(g, s2, sizeof s2, bv, i);
        emit(g, "UCMP", d, s0, s1, s2, NULL);
    }
    r.lv = 0;
    return r;
}

/* all components of a (bool vector) and-ed together */
static struct val all_of(struct gen *g, struct val b)
{
    int n = ncomp(b.t);
    struct val r = v_comp(b, 0);
    for (int i = 1; i < n; i++) r = op2(g, "AND", ty_scalar(GK_BOOL), r, v_comp(b, i));
    return r;
}

/* equality of whole values, any type: a scalar bool */
static struct val equal(struct gen *g, struct val a, struct val b, int ne)
{
    if (!ty_eq(a.t, b.t)) ERR("'%s' of different types", ne ? "!=" : "==");
    if (a.t.k == GK_SAMPLER) ERR("samplers cannot be compared");
    if (ty_regs(a.t) == 1 && !a.t.array && a.t.k != GK_STRUCT && !a.t.cols) {
        if (both_const(a, b)) {
            int eq = 1;
            for (int i = 0; i < ncomp(a.t); i++) if (a.cv[i] != b.cv[i]) eq = 0;
            if (a.t.k == GK_FLOAT) { eq = 1; for (int i = 0; i < ncomp(a.t); i++) if (cv_f(&a, i) != cv_f(&b, i)) eq = 0; }
            return v_bimm(g, ne ? !eq : eq);
        }
        struct val e = op2(g, a.t.k == GK_FLOAT ? "FSEQ" : "USEQ", ty_vec(GK_BOOL, ncomp(a.t)), a, b);
        struct val r = all_of(g, e);
        return ne ? v_not(g, r) : r;
    }
    /* aggregates: compare register by register, with each part's own kind */
    a = v_direct(g, a); b = v_direct(g, b);
    struct val acc = v_bimm(g, 1);
    int reg = 0;
    struct { struct gtype t; int reg; } parts[256]; int np = 0;
    /* flatten into (type, register) leaves */
    struct gtype stack[64]; int sreg[64]; int sp = 0;
    stack[sp] = a.t; sreg[sp++] = 0;
    while (sp) {
        struct gtype t = stack[--sp]; int r = sreg[sp];
        if (t.array) { struct gtype e = elem_type(t); for (int i = t.array - 1; i >= 0; i--) { if (sp >= 64) ERR("value too large to compare"); stack[sp] = e; sreg[sp++] = r + i * ty_regs(e); } continue; }
        if (t.k == GK_STRUCT) {
            int off[64], o = 0;
            for (int i = 0; i < t.st->n; i++) { off[i] = o; o += ty_regs(t.st->f[i].t); }
            for (int i = t.st->n - 1; i >= 0; i--) { if (sp >= 64) ERR("value too large to compare"); stack[sp] = t.st->f[i].t; sreg[sp++] = r + off[i]; }
            continue;
        }
        if (t.cols) { for (int i = 0; i < t.cols; i++) { if (np >= 256) ERR("value too large"); parts[np].t = ty_vec(GK_FLOAT, t.vec); parts[np++].reg = r + i; } continue; }
        if (np >= 256) ERR("value too large");
        parts[np].t = t; parts[np++].reg = r;
    }
    (void)reg;
    for (int i = 0; i < np; i++) {
        struct val pa = a, pb = b;
        pa.t = pb.t = parts[i].t; pa.index += parts[i].reg; pb.index += parts[i].reg;
        ident_swz(&pa); ident_swz(&pb);
        pa.is_const = pb.is_const = 0;
        struct val e = op2(g, parts[i].t.k == GK_FLOAT ? "FSEQ" : "USEQ", ty_vec(GK_BOOL, ncomp(parts[i].t)), pa, pb);
        acc = op2(g, "AND", ty_scalar(GK_BOOL), acc, all_of(g, e));
    }
    return ne ? v_not(g, acc) : acc;
}

/* matrix arithmetic */
static struct val mat_times_vec(struct gen *g, struct val m, struct val v)
{
    if (ncomp(v.t) != m.t.cols) ERR("matrix * vector: sizes do not match");
    m = v_direct(g, m); v = v_direct(g, v);
    struct gtype rt = ty_vec(GK_FLOAT, m.t.vec);
    struct val r = v_temp(g, rt);
    for (int i = 0; i < m.t.cols; i++) {
        char d[96], s0[96], s1[96], s2[96];
        dst_str(g, d, sizeof d, r, 0, m.t.vec);
        src_str(g, s0, sizeof s0, v_col(g, m, i), 0);
        src_str(g, s1, sizeof s1, v_bcast(v_comp(v, i), 4), 0);
        src_str(g, s2, sizeof s2, r, 0);
        if (i == 0) emit(g, "MUL", d, s0, s1, NULL, NULL);
        else emit(g, "MAD", d, s0, s1, s2, NULL);
    }
    r.lv = 0;
    return r;
}

static struct val vec_times_mat(struct gen *g, struct val v, struct val m)
{
    if (ncomp(v.t) != m.t.vec) ERR("vector * matrix: sizes do not match");
    m = v_direct(g, m); v = v_direct(g, v);
    struct val r = v_temp(g, ty_vec(GK_FLOAT, m.t.cols));
    static const char *const dp[] = { NULL, "MUL", "DP2", "DP3", "DP4" };
    for (int i = 0; i < m.t.cols; i++) {
        char d[96], s0[96], s1[96];
        dst_str(g, d, sizeof d, v_comp(r, i), 0, 1);
        src_str(g, s0, sizeof s0, v, 0);
        src_str(g, s1, sizeof s1, v_col(g, m, i), 0);
        emit(g, dp[m.t.vec], d, s0, s1, NULL, NULL);
    }
    r.lv = 0;
    return r;
}

static struct val mat_binary(struct gen *g, const char *op, struct val a, struct val b)
{
    a = v_direct(g, a); b = v_direct(g, b);
    int am = ty_is_matrix(a.t), bm = ty_is_matrix(b.t);
    if (!strcmp(op, "*")) {
        if (am && bm) {
            if (a.t.cols != b.t.vec) ERR("matrix * matrix: sizes do not match");
            struct val r = v_temp(g, ty_mat(b.t.cols, a.t.vec));
            for (int i = 0; i < b.t.cols; i++) {
                struct val col = mat_times_vec(g, a, v_col(g, b, i));
                struct val rc = v_col(g, r, i);
                v_store(g, rc, col);
            }
            r.lv = 0;
            return r;
        }
        if (am && !bm && ncomp(b.t) > 1) return mat_times_vec(g, a, b);
        if (bm && !am && ncomp(a.t) > 1) return vec_times_mat(g, a, b);
    }
    /* component-wise: matrix op matrix, matrix op scalar */
    struct val m = am ? a : b;
    if (am && bm && !ty_eq(a.t, b.t)) ERR("matrix '%s' of different sizes", op);
    if (!(am && bm) && ncomp(am ? b.t : a.t) != 1) ERR("matrix '%s' with a vector", op);
    struct val r = v_temp(g, m.t);
    for (int i = 0; i < m.t.cols; i++) {
        struct val ca = am ? v_col(g, a, i) : v_bcast(a, m.t.vec);
        struct val cb = bm ? v_col(g, b, i) : v_bcast(b, m.t.vec);
        struct val rc = v_col(g, r, i);
        char d[96], s0[96], s1[96];
        dst_str(g, d, sizeof d, rc, 0, m.t.vec);
        src_str(g, s0, sizeof s0, ca, 0);
        if (!strcmp(op, "-")) cb.neg = !cb.neg;
        src_str(g, s1, sizeof s1, cb, 0);
        const char *o = !strcmp(op, "+") || !strcmp(op, "-") ? "ADD" : !strcmp(op, "*") ? "MUL" : !strcmp(op, "/") ? "DIV" : NULL;
        if (!o) ERR("'%s' on matrices", op);
        emit(g, o, d, s0, s1, NULL, NULL);
    }
    r.lv = 0;
    return r;
}

struct val v_binary(struct gen *g, const char *op, struct val a, struct val b)
{
    if (!strcmp(op, "==") || !strcmp(op, "!=")) {
        if (!ty_eq(a.t, b.t) && ty_is_vector(a.t) && ty_is_vector(b.t) && ncomp(a.t) == ncomp(b.t)) {
            if (implicit_ok(b.t, a.t)) b = coerce(g, b, a.t, op); else a = coerce(g, a, b.t, op);
        }
        return equal(g, a, b, op[0] == '!');
    }
    if (!strcmp(op, "&&") || !strcmp(op, "||") || !strcmp(op, "^^")) {
        if (!ty_is_scalar(a.t) || a.t.k != GK_BOOL || !ty_is_scalar(b.t) || b.t.k != GK_BOOL) ERR("'%s' needs bool operands", op);
        if (both_const(a, b)) { int ok; uint32_t r = fold1(GK_BOOL, op, a.cv[0], b.cv[0], &ok); return v_bimm(g, r != 0); }
        return op2(g, op[0] == '&' ? "AND" : op[0] == '|' ? "OR" : "XOR", ty_scalar(GK_BOOL), a, b);
    }
    if (a.t.array || b.t.array || a.t.k == GK_STRUCT || b.t.k == GK_STRUCT || a.t.k == GK_SAMPLER || b.t.k == GK_SAMPLER)
        ERR("'%s' on arrays, structs or samplers", op);
    if (a.t.k == GK_BOOL || b.t.k == GK_BOOL) ERR("'%s' on bool", op);
    /* the common base type: float wins, then uint */
    enum gk k = a.t.k == GK_FLOAT || b.t.k == GK_FLOAT ? GK_FLOAT : a.t.k == GK_UINT || b.t.k == GK_UINT ? GK_UINT : GK_INT;
    int is_shift = !strcmp(op, "<<") || !strcmp(op, ">>");
    int is_bit = is_shift || !strcmp(op, "&") || !strcmp(op, "|") || !strcmp(op, "^") || !strcmp(op, "%");
    if (is_bit && k == GK_FLOAT) ERR("'%s' needs integers", op);
    if (is_shift) k = a.t.k;                          /* a shift keeps the left operand's type */
    else {
        if (a.t.k != k) a = v_convert(g, a, k);
        if (b.t.k != k) b = v_convert(g, b, k);
    }
    if (ty_is_matrix(a.t) || ty_is_matrix(b.t)) return mat_binary(g, op, a, b);
    int na = ncomp(a.t), nb = ncomp(b.t);
    if (na != nb && na != 1 && nb != 1) ERR("'%s' of vectors of different sizes", op);
    int n = na > nb ? na : nb;
    int rel = !strcmp(op, "<") || !strcmp(op, ">") || !strcmp(op, "<=") || !strcmp(op, ">=");
    if (rel && n != 1) ERR("'%s' needs scalars (use lessThan() and friends for vectors)", op);
    a = v_bcast(a, n); b = v_bcast(b, n);
    struct gtype rt = rel ? ty_scalar(GK_BOOL) : ty_vec(k, n);
    if (both_const(a, b)) {
        uint32_t cv[4]; int ok = 1;
        for (int i = 0; i < n && ok; i++) cv[i] = fold1(k, op, a.cv[i], b.cv[i], &ok);
        if (ok && !(k != GK_FLOAT && (!strcmp(op, "/") || !strcmp(op, "%")) && !b.cv[0])) return v_imm(g, rt, cv);
    }
    if (!strcmp(op, "-")) {
        if (k == GK_FLOAT) { b.neg = !b.neg; return op2(g, "ADD", rt, a, b); }
        return op2(g, "UADD", rt, a, op1(g, "INEG", b.t, b));
    }
    if (!strcmp(op, ">")) return op2(g, opcode(g, "<", k), rt, b, a);
    if (!strcmp(op, "<=")) return op2(g, opcode(g, ">=", k), rt, b, a);
    const char *o = opcode(g, op, k);
    if (!o) ERR("unknown operator '%s'", op);
    if (is_shift && b.t.k != GK_INT && b.t.k != GK_UINT) ERR("shift by a non-integer");
    return op2(g, o, rt, a, b);
}

/* ---- symbols ----------------------------------------------------------------------------------- */

static struct sym *sym_find(struct gen *g, const char *name)
{
    for (int i = g->nsyms - 1; i >= 0; i--) {
        struct sym *s = &g->syms[i];
        if (s->frame != 0 && s->frame != g->frame) continue;
        if (!strcmp(s->name, name)) return s;
    }
    return NULL;
}

static struct sym *sym_add(struct gen *g, const char *name, struct gtype t, struct val v, int quals)
{
    for (int i = g->nsyms - 1; i >= 0 && g->syms[i].scope == g->scope && g->syms[i].frame == g->frame; i--)
        if (!strcmp(g->syms[i].name, name)) ERR("'%s' is already declared", name);
    if (!strncmp(name, "gl_", 3) && g->scope > 0) ERR("names starting with gl_ are reserved");
    if (g->nsyms == g->capsyms) {
        int cap = g->capsyms ? g->capsyms * 2 : 256;
        struct sym *n = ar_alloc(g->c, (size_t)cap * sizeof *n);
        if (g->nsyms) memcpy(n, g->syms, (size_t)g->nsyms * sizeof *n);
        g->syms = n; g->capsyms = cap;
    }
    struct sym *s = &g->syms[g->nsyms++];
    s->name = name; s->t = t; s->v = v; s->v.t = t;
    s->scope = g->scope; s->frame = g->frame; s->quals = quals;
    return s;
}

static void scope_push(struct gen *g) { g->scope++; }
static void scope_pop(struct gen *g)
{
    while (g->nsyms > 0 && g->syms[g->nsyms - 1].scope == g->scope && g->syms[g->nsyms - 1].frame == g->frame) g->nsyms--;
    g->scope--;
}

/* ---- the interface: registers and the zgl_compiled tables -------------------------------------- */

static void add_var(struct zgl_var *tab, int *n, const char *name, enum zgl_type t, int array, int slot, int slots, int builtin, int location)
{
    if (*n >= ZGL_MAX_VARS) return;
    struct zgl_var *v = &tab[(*n)++];
    memset(v, 0, sizeof *v);
    snprintf(v->name, sizeof v->name, "%s", name);
    v->type = t; v->array = array; v->slot = slot; v->slots = slots; v->builtin = builtin; v->location = location;
}

static struct val reg_val(enum file f, int index, struct gtype t, int lv)
{
    struct val v;
    memset(&v, 0, sizeof v);
    v.t = t; v.file = f; v.index = index; v.lv = lv;
    ident_swz(&v);
    return v;
}

/* a uniform's leaves (struct members, struct array elements) under GL's names */
static void uniform_leaves(struct gen *g, const char *name, struct gtype t, int base, int builtin)
{
    struct zgl_compiled *o = g->c->out;
    if (t.k == GK_STRUCT) {
        int n = t.array ? t.array : 1, er = ty_elem_regs(t);
        for (int e = 0; e < n; e++) {
            int off = 0;
            for (int i = 0; i < t.st->n; i++) {
                char nm[96];
                if (t.array) snprintf(nm, sizeof nm, "%s[%d].%s", name, e, t.st->f[i].name);
                else snprintf(nm, sizeof nm, "%s.%s", name, t.st->f[i].name);
                uniform_leaves(g, nm, t.st->f[i].t, base + e * er + off, builtin);
                off += ty_regs(t.st->f[i].t);
            }
        }
        return;
    }
    add_var(o->uniforms, &o->nuniforms, name, to_ztype(elem_type(t)), t.array, base, ty_regs(t), builtin, -1);
}

static struct val declare_uniform(struct gen *g, const char *name, struct gtype t, int binding)
{
    struct zgl_compiled *o = g->c->out;
    if (t.k == GK_SAMPLER) {
        int n = t.array ? t.array : 1;
        if (g->nsamp + n > 32) ERR("too many samplers");
        int idx = g->nsamp;
        for (int i = 0; i < n; i++) g->samp_type[idx + i] = t.sampler;
        g->nsamp += n;
        add_var(o->samplers, &o->nsamplers, name, t.sampler, t.array, idx, n, 0, binding);
        struct val v = reg_val(F_SAMP, idx, t, 0);
        v.samp = idx;
        return v;
    }
    int regs = ty_regs(t);
    if (g->nconst + regs > ZGL_MAX_CONSTS) ERR("too many uniforms");
    int idx = g->nconst;
    g->nconst += regs;
    uniform_leaves(g, name, t, idx, 0);
    return reg_val(F_CONST, idx, t, 0);
}

static int find_vs_varying(struct gen *g, const char *name)
{
    if (!g->c->vs) return -1;
    for (int i = 0; i < g->c->vs->nvaryings; i++)
        if (!strcmp(g->c->vs->varyings[i].name, name)) return g->c->vs->varyings[i].slot;
    return -1;
}

static int next_generic(struct gen *g)
{
    int slot = 0;
    struct zgl_compiled *o = g->c->out;
    for (int i = 0; i < o->nvaryings; i++) if (o->varyings[i].slot + o->varyings[i].slots > slot) slot = o->varyings[i].slot + o->varyings[i].slots;
    return slot;
}

static void io_decl(struct gen *g, int is_in, int first, int n, const char *sem, int sem_index, const char *interp)
{
    char (*tab)[96] = is_in ? g->in_decl : g->out_decl;
    if (first + n > 96) ERR("too many inputs or outputs");
    if (n > 1) {
        /* one register at a time: the host translation has no indirect I/O, so
         * dynamic indexing of these is done with branches (val.ind == 2) */
        for (int i = 0; i < n; i++)
            snprintf(tab[first + i], 96, "DCL %s[%d], %s[%d]%s\n", is_in ? "IN" : "OUT", first + i, sem, (sem_index >= 0 ? sem_index : 0) + i, interp ? interp : "");
    } else {
        if (sem_index >= 0) snprintf(tab[first], 96, "DCL %s[%d], %s[%d]%s\n", is_in ? "IN" : "OUT", first, sem, sem_index, interp ? interp : "");
        else if (sem) snprintf(tab[first], 96, "DCL %s[%d], %s%s\n", is_in ? "IN" : "OUT", first, sem, interp ? interp : "");
        else snprintf(tab[first], 96, "DCL %s[%d]\n", is_in ? "IN" : "OUT", first);
    }
}


static struct val declare_attribute(struct gen *g, const char *name, struct gtype t, int location, int builtin_loc)
{
    if (g->c->stage != GL_VERTEX_SHADER) ERR("'%s': attributes are vertex shader inputs", name);
    if (t.k == GK_STRUCT || t.k == GK_SAMPLER || t.k == GK_BOOL) ERR("'%s': attributes are numbers, vectors or matrices", name);
    int n = ty_regs(t);
    int idx = g->nin;
    g->nin += n;
    for (int i = 0; i < n; i++) snprintf(g->in_decl[idx + i], 96, "DCL IN[%d]\n", idx + i);
    struct zgl_compiled *o = g->c->out;
    add_var(o->attribs, &o->nattribs, name, to_ztype(elem_type(t)), t.array, idx, n, builtin_loc >= 0 ? builtin_loc + 1 : 0, location);
    return reg_val(F_IN, idx, t, 0);
}

static const char *interp_of(int quals, struct gtype t)
{
    int flat = (quals & Q_FLAT) || t.k == GK_INT || t.k == GK_UINT || t.k == GK_BOOL;
    const char *s = flat ? ", CONSTANT" : (quals & Q_NOPERSPECTIVE) ? ", LINEAR" : ", PERSPECTIVE";
    if (quals & Q_CENTROID) return flat ? ", CONSTANT, CENTROID" : (quals & Q_NOPERSPECTIVE) ? ", LINEAR, CENTROID" : ", PERSPECTIVE, CENTROID";
    return s;
}

static struct val declare_varying(struct gen *g, const char *name, struct gtype t, int quals)
{
    struct zgl_compiled *o = g->c->out;
    if (t.k == GK_STRUCT || t.k == GK_SAMPLER) ERR("'%s': varyings cannot be structs or samplers", name);
    if (t.k == GK_BOOL) ERR("'%s': varyings cannot be bool", name);
    if ((t.k == GK_INT || t.k == GK_UINT) && g->c->stage == GL_FRAGMENT_SHADER && !(quals & Q_FLAT)) ERR("'%s': integer inputs must be flat", name);
    int n = ty_regs(t);
    int flat = (quals & Q_FLAT) || t.k == GK_INT || t.k == GK_UINT;
    if (g->c->stage == GL_VERTEX_SHADER) {
        int slot = next_generic(g);
        int idx = g->nout;
        g->nout += n;
        io_decl(g, 0, idx, n, "GENERIC", slot, NULL);
        add_var(o->varyings, &o->nvaryings, name, to_ztype(elem_type(t)), t.array, slot, n, 0, -1);
        o->varyings[o->nvaryings - 1].flat = flat;
        return reg_val(F_OUT, idx, t, 1);
    }
    int slot = find_vs_varying(g, name);
    if (slot < 0) slot = 32 + g->nin;               /* nothing feeds it: a spare index */
    int idx = g->nin;
    g->nin += n;
    io_decl(g, 1, idx, n, "GENERIC", slot, interp_of(quals, t));
    add_var(o->varyings, &o->nvaryings, name, to_ztype(elem_type(t)), t.array, slot, n, 0, -1);
    o->varyings[o->nvaryings - 1].flat = flat;
    return reg_val(F_IN, idx, t, 0);
}

static struct val declare_frag_output(struct gen *g, const char *name, struct gtype t, int location, int index)
{
    if (t.k == GK_STRUCT || t.k == GK_SAMPLER || t.k == GK_BOOL || t.cols) ERR("'%s': fragment outputs are scalars or vectors", name);
    int n = t.array ? t.array : 1;
    if (index == 1) {
        /* dual-source blending: the second colour of location 0 is COLOR[1] */
        if (location != 0 || n != 1) ERR("'%s': a second-source output is location 0, not an array", name);
        if (g->dual_src) ERR("'%s': two second-source outputs", name);
        g->dual_src = 1;
        int idx = g->nout++;
        io_decl(g, 0, idx, 1, "COLOR", 1, NULL);
        struct zgl_compiled *o = g->c->out;
        if (o->noutputs < 8) {
            struct zgl_var *v = &o->outputs[o->noutputs++];
            memset(v, 0, sizeof *v);
            snprintf(v->name, sizeof v->name, "%s", name);
            v->type = to_ztype(t); v->slot = 0; v->slots = 1; v->location = 0; v->builtin = 1;   /* builtin 1: index 1 */
        }
        return reg_val(F_OUT, idx, t, 1);
    }
    int loc = location;
    if (loc < 0) {
        loc = 0;
        while (loc < 8 && (g->color_used & (1u << loc))) loc++;
    }
    if (loc + n > 8) ERR("'%s': fragment output location out of range", name);
    for (int i = 0; i < n; i++) {
        if (g->color_used & (1u << (loc + i))) ERR("'%s': output location %d is already used", name, loc + i);
        g->color_used |= 1u << (loc + i);
    }
    int idx = g->nout;
    g->nout += n;
    io_decl(g, 0, idx, n, "COLOR", loc, NULL);
    struct zgl_compiled *o = g->c->out;
    if (o->noutputs < 8) {
        struct zgl_var *v = &o->outputs[o->noutputs++];
        memset(v, 0, sizeof *v);
        snprintf(v->name, sizeof v->name, "%s", name);
        v->type = to_ztype(elem_type(t)); v->array = t.array; v->slot = loc; v->slots = n; v->location = location;
    }
    return reg_val(F_OUT, idx, t, 1);
}

/* ---- built-in variables -------------------------------------------------------------------------- */

static struct gstruct *light_struct, *material_struct, *lightmodel_struct;

static struct gstruct *make_struct(struct gen *g, const char *name, const char *const *fields, const struct gtype *types, int n)
{
    struct gstruct *s = ar_alloc(g->c, sizeof *s);
    s->name = name;
    s->f = ar_alloc(g->c, (size_t)n * sizeof *s->f);
    for (int i = 0; i < n; i++) { s->f[i].name = fields[i]; s->f[i].t = types[i]; }
    s->n = n;
    return s;
}

static struct sym *global_sym(struct gen *g, const char *name, struct gtype t, struct val v)
{
    int scope = g->scope, frame = g->frame;
    g->scope = 0; g->frame = 0;
    struct sym *s = sym_add(g, name, t, v, 0);
    g->scope = scope; g->frame = frame;
    return s;
}

static struct sym *builtin_var(struct gen *g, const char *name)
{
    struct glc *c = g->c;
    int vs = c->stage == GL_VERTEX_SHADER, fs = !vs;
    struct gtype f = ty_scalar(GK_FLOAT), v2 = ty_vec(GK_FLOAT, 2), v3 = ty_vec(GK_FLOAT, 3), v4 = ty_vec(GK_FLOAT, 4);
    struct gtype m4 = ty_mat(4, 4), m3 = ty_mat(3, 3);
    struct zgl_compiled *o = c->out;
    /* compat attributes */
    static const struct { const char *n; int loc; int size; } attrs[] = {
        { "gl_Vertex", ZGL_ATTR_VERTEX, 4 }, { "gl_Normal", ZGL_ATTR_NORMAL, 3 }, { "gl_Color", ZGL_ATTR_COLOR, 4 },
        { "gl_SecondaryColor", 4, 4 }, { "gl_FogCoord", 5, 1 },
        { "gl_MultiTexCoord0", ZGL_ATTR_TEXCOORD, 4 }, { "gl_MultiTexCoord1", ZGL_ATTR_TEXCOORD + 1, 4 },
        { "gl_MultiTexCoord2", ZGL_ATTR_TEXCOORD + 2, 4 }, { "gl_MultiTexCoord3", ZGL_ATTR_TEXCOORD + 3, 4 },
        { "gl_MultiTexCoord4", ZGL_ATTR_TEXCOORD + 4, 4 }, { "gl_MultiTexCoord5", ZGL_ATTR_TEXCOORD + 5, 4 },
        { "gl_MultiTexCoord6", ZGL_ATTR_TEXCOORD + 6, 4 }, { "gl_MultiTexCoord7", ZGL_ATTR_TEXCOORD + 7, 4 },
    };
    if (vs)
        for (size_t i = 0; i < sizeof attrs / sizeof attrs[0]; i++)
            if (!strcmp(name, attrs[i].n)) {
                struct gtype t = ty_vec(GK_FLOAT, attrs[i].size);
                return global_sym(g, name, t, declare_attribute(g, name, t, -1, attrs[i].loc));
            }
    /* state uniforms */
    static const struct { const char *n; int mat; int array; int state; } st[] = {
        { "gl_ModelViewProjectionMatrix", 4, 0, ZGL_STATE_MVP }, { "gl_ModelViewMatrix", 4, 0, ZGL_STATE_MV },
        { "gl_ProjectionMatrix", 4, 0, ZGL_STATE_PROJ }, { "gl_NormalMatrix", 3, 0, ZGL_STATE_NORMAL },
        { "gl_ModelViewMatrixInverseTranspose", 4, 0, ZGL_STATE_MV_INV_T }, { "gl_TextureMatrix", 4, 1, ZGL_STATE_TEXTURE0_MATRIX },
    };
    for (size_t i = 0; i < sizeof st / sizeof st[0]; i++)
        if (!strcmp(name, st[i].n)) {
            struct gtype t = st[i].mat == 4 ? m4 : m3;
            t.array = st[i].array;
            int idx = g->nconst;
            g->nconst += ty_regs(t);
            add_var(o->uniforms, &o->nuniforms, name, to_ztype(elem_type(t)), t.array, idx, ty_regs(t), st[i].state, -1);
            return global_sym(g, name, t, reg_val(F_CONST, idx, t, 0));
        }
    if (!strcmp(name, "gl_LightSource")) {
        if (!light_struct) {
            static const char *const fn[] = { "ambient", "diffuse", "specular", "position", "spotDirection", "spotExponent", "spotCutoff",
                                              "spotCosCutoff", "constantAttenuation", "linearAttenuation", "quadraticAttenuation" };
            struct gtype ft[11] = { v4, v4, v4, v4, v3, f, f, f, f, f, f };
            light_struct = make_struct(g, "gl_LightSourceParameters", fn, ft, 11);
        }
        struct gtype t = ty_scalar(GK_STRUCT);
        t.st = light_struct;
        t.array = ZGL_MAX_LIGHTS;
        int idx = g->nconst;
        g->nconst += ty_regs(t);
        for (int i = 0; i < ZGL_MAX_LIGHTS; i++) {
            char nm[32];
            snprintf(nm, sizeof nm, "gl_LightSource[%d]", i);
            add_var(o->uniforms, &o->nuniforms, nm, T_VEC4, 11, idx + i * 11, 11, ZGL_STATE_LIGHT0 + i, -1);
        }
        return global_sym(g, name, t, reg_val(F_CONST, idx, t, 0));
    }
    if (!strcmp(name, "gl_FrontMaterial") || !strcmp(name, "gl_BackMaterial")) {
        struct sym *s = sym_find(g, "gl_FrontMaterial");
        if (!s) {
            if (!material_struct) {
                static const char *const fn[] = { "emission", "ambient", "diffuse", "specular", "shininess" };
                struct gtype ft[5] = { v4, v4, v4, v4, f };
                material_struct = make_struct(g, "gl_MaterialParameters", fn, ft, 5);
            }
            struct gtype t = ty_scalar(GK_STRUCT);
            t.st = material_struct;
            int idx = g->nconst;
            g->nconst += 5;
            add_var(o->uniforms, &o->nuniforms, "gl_FrontMaterial", T_VEC4, 5, idx, 5, ZGL_STATE_MATERIAL_FRONT, -1);
            s = global_sym(g, "gl_FrontMaterial", t, reg_val(F_CONST, idx, t, 0));
        }
        if (!strcmp(name, "gl_BackMaterial")) return global_sym(g, "gl_BackMaterial", s->t, s->v);
        return s;
    }
    if (!strcmp(name, "gl_LightModel")) {
        if (!lightmodel_struct) {
            static const char *const fn[] = { "ambient" };
            struct gtype ft[1] = { v4 };
            lightmodel_struct = make_struct(g, "gl_LightModelParameters", fn, ft, 1);
        }
        struct gtype t = ty_scalar(GK_STRUCT);
        t.st = lightmodel_struct;
        int idx = g->nconst++;
        add_var(o->uniforms, &o->nuniforms, "gl_LightModelAmbient", T_VEC4, 0, idx, 1, ZGL_STATE_LIGHTMODEL_AMBIENT, -1);
        return global_sym(g, name, t, reg_val(F_CONST, idx, t, 0));
    }
    /* implementation limits */
    static const struct { const char *n; int v; } lim[] = {
        { "gl_MaxLights", ZGL_MAX_LIGHTS }, { "gl_MaxClipPlanes", 8 }, { "gl_MaxTextureUnits", ZGL_MAX_UNITS },
        { "gl_MaxTextureCoords", 8 }, { "gl_MaxVertexAttribs", ZGL_MAX_ATTRIBS }, { "gl_MaxVertexUniformComponents", ZGL_MAX_CONSTS * 4 },
        { "gl_MaxVaryingFloats", 64 }, { "gl_MaxVaryingComponents", 64 }, { "gl_MaxVertexTextureImageUnits", 16 },
        { "gl_MaxCombinedTextureImageUnits", 32 }, { "gl_MaxTextureImageUnits", 16 }, { "gl_MaxFragmentUniformComponents", ZGL_MAX_CONSTS * 4 },
        { "gl_MaxDrawBuffers", 8 }, { "gl_MaxClipDistances", 8 }, { "gl_MaxVertexUniformVectors", ZGL_MAX_CONSTS },
        { "gl_MaxFragmentUniformVectors", ZGL_MAX_CONSTS }, { "gl_MaxVaryingVectors", 16 }, { "gl_MaxVertexOutputComponents", 64 },
        { "gl_MaxFragmentInputComponents", 64 }, { "gl_MinProgramTexelOffset", -8 }, { "gl_MaxProgramTexelOffset", 7 },
    };
    for (size_t i = 0; i < sizeof lim / sizeof lim[0]; i++)
        if (!strcmp(name, lim[i].n)) return global_sym(g, name, ty_scalar(GK_INT), v_iimm(g, lim[i].v));
    if (vs) {
        if (!strcmp(name, "gl_PointSize")) {
            int idx = g->nout++;
            io_decl(g, 0, idx, 1, "PSIZE", -1, NULL);
            return global_sym(g, name, f, reg_val(F_OUT, idx, f, 1));
        }
        if (!strcmp(name, "gl_InstanceID")) {
            /* zgl has no instanced draws yet, so it is always 0; and the
             * INSTANCEID system value would make virglrenderer (1.3.0) ask
             * the host for ARB_draw_instanced, which macOS's core profile
             * lacks and which fails the whole context. When instancing
             * comes, this becomes a hidden per-instance attribute. */
            return global_sym(g, name, ty_scalar(GK_INT), v_iimm(g, 0));
        }
        if (!strcmp(name, "gl_VertexID")) {
            int idx = g->nsv++;
            snprintf(g->sv_decl[idx], 64, "DCL SV[%d], VERTEXID\n", idx);
            return global_sym(g, name, ty_scalar(GK_INT), reg_val(F_SV, idx, ty_scalar(GK_INT), 0));
        }
        if (!strcmp(name, "gl_ClipVertex")) return global_sym(g, name, v4, v_temp(g, v4));
        if (!strcmp(name, "gl_FrontColor") || !strcmp(name, "gl_BackColor") || !strcmp(name, "gl_FrontSecondaryColor") ||
            !strcmp(name, "gl_BackSecondaryColor")) {
            if (strstr(name, "Back")) return global_sym(g, name, v4, v_temp(g, v4));     /* no two-sided colour */
            return global_sym(g, name, v4, declare_varying(g, name, v4, 0));
        }
    }
    if (fs) {
        if (!strcmp(name, "gl_FragCoord")) {
            int idx = g->nin++;
            io_decl(g, 1, idx, 1, "POSITION", -1, ", LINEAR");
            return global_sym(g, name, v4, reg_val(F_IN, idx, v4, 0));
        }
        if (!strcmp(name, "gl_FrontFacing")) {
            int idx = g->nin++;
            io_decl(g, 1, idx, 1, "FACE", -1, ", CONSTANT");
            /* FACE is a float, > 0 for the front: compared where it is read (Q_FACE) */
            struct sym *s = global_sym(g, name, ty_scalar(GK_BOOL), reg_val(F_IN, idx, f, 0));
            s->quals = Q_FACE;
            return s;
        }
        if (!strcmp(name, "gl_PointCoord")) {
            int idx = g->nin++;
            io_decl(g, 1, idx, 1, "PCOORD", -1, ", LINEAR");
            return global_sym(g, name, v2, reg_val(F_IN, idx, v2, 0));
        }
        if (!strcmp(name, "gl_FragColor")) {
            if (g->color_used & 1) ERR("gl_FragColor together with other fragment outputs");
            g->color_used |= 1;
            g->writes_fragcolor = 1;
            int idx = g->nout++;
            io_decl(g, 0, idx, 1, "COLOR", -1, NULL);
            return global_sym(g, name, v4, reg_val(F_OUT, idx, v4, 1));
        }
        if (!strcmp(name, "gl_FragData")) {
            if (g->color_used) ERR("gl_FragData together with other fragment outputs");
            g->color_used = 0xFF;
            int idx = g->nout;
            g->nout += 8;
            io_decl(g, 0, idx, 8, "COLOR", 0, NULL);
            struct gtype t = v4; t.array = 8;
            return global_sym(g, name, t, reg_val(F_OUT, idx, t, 1));
        }
        if (!strcmp(name, "gl_FragDepth")) {
            int idx = g->nout++;
            io_decl(g, 0, idx, 1, "POSITION", -1, NULL);
            struct val v = reg_val(F_OUT, idx, f, 1);
            for (int i = 0; i < 4; i++) v.swz[i] = 2;      /* the depth is .z */
            return global_sym(g, name, f, v);
        }
        if (!strcmp(name, "gl_Color")) { struct sym *s = sym_find(g, "gl_FrontColor"); if (!s) s = global_sym(g, "gl_FrontColor", v4, declare_varying(g, "gl_FrontColor", v4, 0)); return s; }
        if (!strcmp(name, "gl_SecondaryColor")) { struct sym *s = sym_find(g, "gl_FrontSecondaryColor"); if (!s) s = global_sym(g, "gl_FrontSecondaryColor", v4, declare_varying(g, "gl_FrontSecondaryColor", v4, 0)); return s; }
    }
    if (!strcmp(name, "gl_TexCoord")) { struct gtype t = v4; t.array = 8; return global_sym(g, name, t, declare_varying(g, name, t, 0)); }
    if (!strcmp(name, "gl_FogFragCoord")) return global_sym(g, name, f, declare_varying(g, name, f, 0));
    return NULL;
}

/* ---- constant expressions ------------------------------------------------------------------------- */

static int const_int(struct gen *g, struct node *n, const char *what)
{
    /* evaluated in a scratch stream: only the constant result matters */
    size_t len = g->len; int ninst = g->ninst;
    struct val v = expr(g, n);
    g->len = len; if (g->code) g->code[len] = 0; g->ninst = ninst;
    if (!v.is_const || !ty_is_scalar(v.t) || (v.t.k != GK_INT && v.t.k != GK_UINT)) ERR("%s must be a constant integer expression", what);
    return (int)v.cv[0];
}

/* ---- expressions ---------------------------------------------------------------------------------- */

static struct val call(struct gen *g, struct node *n);
static void stmt(struct gen *g, struct node *n);

static int has_side_effects(struct node *n)
{
    if (!n) return 0;
    switch (n->k) {
    case N_ASSIGN: case N_PREINC: case N_PREDEC: case N_POSTINC: case N_POSTDEC: return 1;
    case N_CALL: if (!is_builtin_func(n->name)) return 1; break;
    default: break;
    }
    if (has_side_effects(n->a) || has_side_effects(n->b) || has_side_effects(n->c)) return 1;
    for (int i = 0; i < n->n; i++) if (has_side_effects(n->l[i])) return 1;
    return 0;
}

static struct val field(struct gen *g, struct val base, const char *f)
{
    if (base.t.k == GK_STRUCT && !base.t.array) {
        int off = 0;
        for (int i = 0; i < base.t.st->n; i++) {
            if (!strcmp(base.t.st->f[i].name, f)) {
                struct val r = base;
                r.t = base.t.st->f[i].t;
                r.index += off;
                ident_swz(&r);
                r.is_const = 0;
                return r;
            }
            off += ty_regs(base.t.st->f[i].t);
        }
        ERR("'%s' has no member '%s'", base.t.st->name, f);
    }
    if (ty_is_vector(base.t)) {
        int idx[4], n = (int)strlen(f);
        if (n < 1 || n > 4) ERR("bad swizzle '.%s'", f);
        const char *sets[3] = { "xyzw", "rgba", "stpq" };
        int set = -1;
        for (int i = 0; i < n; i++) {
            int k = -1, s;
            for (s = 0; s < 3 && k < 0; s++) { const char *p = strchr(sets[s], f[i]); if (p) { k = (int)(p - sets[s]); break; } }
            if (k < 0) ERR("bad swizzle '.%s'", f);
            if (set >= 0 && s != set) ERR("swizzle '.%s' mixes component sets", f);
            set = s;
            if (k >= ncomp(base.t)) ERR("swizzle '.%s' out of range for a %d-component value", f, ncomp(base.t));
            idx[i] = k;
        }
        return v_swizzle(base, idx, n);
    }
    char b[48];
    ERR("'.%s' on a %s", f, ty_name(base.t, b, sizeof b));
}

/* base[idx] */
static struct val index_val(struct gen *g, struct val base, struct val idx)
{
    if (!ty_is_scalar(idx.t) || (idx.t.k != GK_INT && idx.t.k != GK_UINT)) ERR("an index must be an integer");
    if (base.t.array || ty_is_matrix(base.t)) {
        struct gtype et = base.t.array ? elem_type(base.t) : ty_vec(GK_FLOAT, base.t.vec);
        int count = base.t.array ? base.t.array : base.t.cols;
        int er = base.t.array ? ty_elem_regs(base.t) : 1;
        struct val r = base;
        r.t = et;
        ident_swz(&r);
        r.is_const = 0;
        if (idx.is_const) {
            int i = (int)idx.cv[0];
            if (i < 0 || (count > 0 && i >= count)) ERR("index %d out of range [0, %d)", i, count);
            r.index += i * er;
            if (base.is_const && !base.t.array) { memcpy(r.cv, base.cv + i * base.t.vec, (size_t)base.t.vec * 4); r.is_const = 1; }
            return r;
        }
        if (base.file == F_IMM || base.file == F_SAMP) {
            if (base.file == F_SAMP) ERR("samplers can only be indexed with constants");
            base = v_copy(g, base);
            r = base; r.t = et; ident_swz(&r); r.is_const = 0;
        }
        if ((base.file == F_IN || base.file == F_OUT) && !base.ind) {
            struct val iv = v_copy(g, v_convert(g, idx, GK_INT));
            r.ind = 2;
            r.ind_reg = iv.index;
            r.ind_count = count;
            r.ind_stride = er;
            return r;
        }
        if ((base.file == F_IN || base.file == F_OUT) && base.ind == 2) ERR("indexing an element of an input/output array twice at run time is not supported");
        if (base.file == F_TEMP && !base.arr) {
            /* not declared as an array (a matrix in a plain register run): copy into one */
            struct val cp = v_copy(g, base);
            if (!cp.arr) ERR("internal: indexing a register that is not an array");
            r = cp; r.t = et; ident_swz(&r); r.is_const = 0; r.lv = 0;
        }
        struct val off = er == 1 ? v_convert(g, idx, GK_INT) : op2(g, "UMUL", ty_scalar(GK_INT), v_convert(g, idx, GK_INT), v_iimm(g, er));
        if (base.ind) off = op2(g, "UADD", ty_scalar(GK_INT), off, reg_val(F_TEMP, base.ind_reg, ty_scalar(GK_INT), 0));
        off = v_copy(g, off);
        r.ind = 1;
        r.ind_reg = off.index;
        /* the swizzle of the copy is .x: keep the register only */
        return r;
    }
    if (ty_is_vector(base.t)) {
        int n = ncomp(base.t);
        if (idx.is_const) {
            int i = (int)idx.cv[0];
            if (i < 0 || i >= n) ERR("component %d out of range", i);
            struct val r = v_comp(base, i);
            r.lv = base.lv;
            return r;
        }
        /* a dynamic component: select it */
        base = v_direct(g, base);
        struct val r = v_comp(base, 0);
        struct val ii = v_convert(g, idx, GK_INT);
        for (int i = 1; i < n; i++) {
            struct val eq = op2(g, "USEQ", ty_scalar(GK_BOOL), ii, v_iimm(g, i));
            r = op3(g, "UCMP", ty_scalar(base.t.k), eq, v_comp(base, i), r);
        }
        r.lv = 0;
        return r;
    }
    ERR("this value cannot be indexed");
}

static int is_lvalue(struct val v) { return v.lv && !v.is_const && (v.file == F_TEMP || v.file == F_OUT); }

static void assign(struct gen *g, struct val dst, struct val src, int line)
{
    g->c->line = line;
    if (!is_lvalue(dst)) ERR("assignment to something that is not a variable");
    src = coerce(g, src, dst.t, "assignment");
    v_store(g, dst, src);
}

/* the target of an assignment where a vector is indexed dynamically: store per component */
static int assign_dyn_component(struct gen *g, struct node *lhs, struct val val)
{
    if (lhs->k != N_INDEX) return 0;
    struct val base = expr(g, lhs->a);
    if (!ty_is_vector(base.t) || ncomp(base.t) == 1) return 0;
    struct val idx = expr(g, lhs->b);
    if (idx.is_const) return 0;
    if (!is_lvalue(base)) ERR("assignment to something that is not a variable");
    val = coerce(g, val, ty_scalar(base.t.k), "assignment");
    struct val ii = v_convert(g, idx, GK_INT);
    for (int i = 0; i < ncomp(base.t); i++) {
        struct val eq = op2(g, "USEQ", ty_scalar(GK_BOOL), ii, v_iimm(g, i));
        struct val dc = v_comp(base, i);
        dc.lv = 1;
        v_store(g, dc, op3(g, "UCMP", ty_scalar(base.t.k), eq, val, v_direct(g, v_comp(base, i))));
    }
    return 1;
}

static struct val incdec(struct gen *g, struct node *n)
{
    struct val v = expr(g, n->a);
    if (!is_lvalue(v)) ERR("'++'/'--' on something that is not a variable");
    if (!ty_is_numeric(v.t)) ERR("'++'/'--' on a non-number");
    int up = n->k == N_PREINC || n->k == N_POSTINC;
    struct val one = v.t.k == GK_FLOAT ? v_fimm(g, 1) : v.t.k == GK_UINT ? v_uimm(g, 1) : v_iimm(g, 1);
    struct val old = (n->k == N_POSTINC || n->k == N_POSTDEC) ? v_copy(g, v) : v;
    struct val nv = v_binary(g, up ? "+" : "-", v_direct(g, v), one);
    v_store(g, v, nv);
    old.lv = 0;
    if (n->k == N_PREINC || n->k == N_PREDEC) { struct val r = v; r.lv = 0; return v_direct(g, r); }
    return old;
}

static struct val expr(struct gen *g, struct node *n)
{
    g->c->line = n->line;
    switch (n->k) {
    case N_INTLIT: return v_iimm(g, (int)n->u);
    case N_UINTLIT: return v_uimm(g, n->u);
    case N_FLOATLIT: return v_fimm(g, (float)n->f);
    case N_BOOLLIT: return v_bimm(g, (int)n->u);
    case N_IDENT: {
        struct sym *s = sym_find(g, n->name);
        if (!s) s = builtin_var(g, n->name);
        if (!s) ERR("'%s' undeclared", n->name);
        struct val v = s->v;
        v.t = s->t;
        if (s->quals & Q_FACE) { struct val face = s->v; face.t = ty_scalar(GK_FLOAT); return op2(g, "FSLT", ty_scalar(GK_BOOL), v_fimm(g, 0), face); }
        if (s->quals & (Q_CONST | Q_UNIFORM)) v.lv = 0;
        return v;
    }
    case N_FIELD:
        /* instance.member of a uniform block is a name of its own */
        if (n->a->k == N_IDENT && !sym_find(g, n->a->name)) {
            char nm[160];
            snprintf(nm, sizeof nm, "%s.%s", n->a->name, n->name);
            struct sym *s = sym_find(g, nm);
            if (s) { struct val v = s->v; v.t = s->t; v.lv = 0; return v; }
        }
        return field(g, expr(g, n->a), n->name);
    case N_INDEX: { struct val b = expr(g, n->a); struct val i = expr(g, n->b); return index_val(g, b, i); }
    case N_CALL: return call(g, n);
    case N_COMMA: expr(g, n->a); return expr(g, n->b);
    case N_PREINC: case N_PREDEC: case N_POSTINC: case N_POSTDEC: return incdec(g, n);
    case N_UNARY: {
        struct val a = expr(g, n->a);
        if (!strcmp(n->op, "+")) { if (!ty_is_numeric(a.t)) ERR("unary '+' on a non-number"); return a; }
        if (!strcmp(n->op, "-")) {
            if (!ty_is_numeric(a.t)) ERR("unary '-' on a non-number");
            if (a.is_const && !a.t.cols) {
                uint32_t cv[4];
                for (int i = 0; i < ncomp(a.t); i++) cv[i] = a.t.k == GK_FLOAT ? f2u(-cv_f(&a, i)) : (uint32_t)-(int64_t)a.cv[i];
                return v_imm(g, a.t, cv);
            }
            if (a.t.cols) return mat_binary(g, "*", a, v_fimm(g, -1));
            if (a.t.k == GK_FLOAT) { a = v_direct(g, a); a.neg = !a.neg; a.lv = 0; return a; }
            return op1(g, "INEG", a.t, a);
        }
        if (!strcmp(n->op, "!")) { if (!ty_is_scalar(a.t) || a.t.k != GK_BOOL) ERR("'!' needs a bool"); return v_not(g, a); }
        if (!strcmp(n->op, "~")) { if (!ty_is_vector(a.t) || (a.t.k != GK_INT && a.t.k != GK_UINT)) ERR("'~' needs an integer"); return v_not(g, a); }
        ERR("unknown operator '%s'", n->op);
    }
    case N_BINARY: {
        /* && and || skip the right side when it has effects and the left decides */
        if ((!strcmp(n->op, "&&") || !strcmp(n->op, "||")) && has_side_effects(n->b)) {
            struct val a = expr(g, n->a);
            if (!ty_is_scalar(a.t) || a.t.k != GK_BOOL) ERR("'%s' needs bool operands", n->op);
            struct val r = v_copy(g, a);
            char s[96];
            src_str(g, s, sizeof s, n->op[0] == '&' ? a : v_not(g, a), 0);
            emit(g, "UIF", NULL, s, NULL, NULL, NULL);
            struct val b = expr(g, n->b);
            if (!ty_is_scalar(b.t) || b.t.k != GK_BOOL) ERR("'%s' needs bool operands", n->op);
            v_store(g, r, b);
            emit(g, "ENDIF", NULL, NULL, NULL, NULL, NULL);
            r.lv = 0;
            return r;
        }
        struct val a = expr(g, n->a);
        struct val b = expr(g, n->b);
        g->c->line = n->line;
        return v_binary(g, n->op, a, b);
    }
    case N_ASSIGN: {
        struct val rhs = expr(g, n->b);
        if (!strcmp(n->op, "=")) {
            if (assign_dyn_component(g, n->a, rhs)) return rhs;
            struct val lhs = expr(g, n->a);
            assign(g, lhs, rhs, n->line);
            struct val r = lhs; r.lv = 0;
            return r;
        }
        char op[4];
        snprintf(op, sizeof op, "%.*s", (int)strlen(n->op) - 1, n->op);
        struct val lhs = expr(g, n->a);
        if (!is_lvalue(lhs)) ERR("assignment to something that is not a variable");
        struct val r = v_binary(g, op, v_direct(g, lhs), rhs);
        assign(g, lhs, r, n->line);
        struct val res = lhs; res.lv = 0;
        return res;
    }
    case N_COND: {
        struct val cnd = expr(g, n->a);
        if (!ty_is_scalar(cnd.t) || cnd.t.k != GK_BOOL) ERR("'?:' needs a bool condition");
        if (cnd.is_const) return cnd.cv[0] ? expr(g, n->b) : expr(g, n->c);
        if (!has_side_effects(n->b) && !has_side_effects(n->c)) {
            struct val a = expr(g, n->b), b = expr(g, n->c);
            if (!ty_eq(a.t, b.t)) { if (implicit_ok(b.t, a.t)) b = coerce(g, b, a.t, "?:"); else a = coerce(g, a, b.t, "?:"); }
            return v_select(g, cnd, a, b);
        }
        /* with effects: real branches into a temp */
        cnd = v_copy(g, cnd);
        char s[96];
        src_str(g, s, sizeof s, cnd, 0);
        emit(g, "UIF", NULL, s, NULL, NULL, NULL);
        struct val a = expr(g, n->b);
        struct val r = v_temp(g, a.t);
        v_store(g, r, a);
        emit(g, "ELSE", NULL, NULL, NULL, NULL, NULL);
        struct val b = expr(g, n->c);
        v_store(g, r, coerce(g, b, a.t, "?:"));
        emit(g, "ENDIF", NULL, NULL, NULL, NULL, NULL);
        r.lv = 0;
        return r;
    }
    default:
        ERR("not an expression");
    }
}

/* ---- constructors ------------------------------------------------------------------------------ */

/* the scalar components of a list of values, in order (matrices column-major) */
struct comps { struct val v[64]; int n; };

static void gather(struct gen *g, struct comps *cs, struct val a)
{
    if (a.t.array || a.t.k == GK_STRUCT || a.t.k == GK_SAMPLER) ERR("constructor argument cannot be an array, struct or sampler");
    a = v_direct(g, a);
    if (a.t.cols) {
        for (int c = 0; c < a.t.cols; c++) { struct val col = v_col(g, a, c); for (int r = 0; r < a.t.vec; r++) { if (cs->n >= 64) ERR("too many constructor components"); cs->v[cs->n++] = v_comp(col, r); } }
        return;
    }
    for (int i = 0; i < ncomp(a.t); i++) { if (cs->n >= 64) ERR("too many constructor components"); cs->v[cs->n++] = v_comp(a, i); }
}

static struct val construct(struct gen *g, struct gtype t, struct val *args, int n)
{
    if (t.k == GK_VOID || t.k == GK_SAMPLER) ERR("cannot construct this type");
    if (n == 0) ERR("a constructor needs arguments");
    if (t.array) {
        if (t.array < 0) t.array = n;
        if (n != t.array) ERR("array constructor: %d elements, %d given", t.array, n);
        struct gtype et = elem_type(t);
        struct val r = v_temp(g, t);
        for (int i = 0; i < n; i++) {
            struct val e = r; e.t = et; e.index += i * ty_regs(et); ident_swz(&e); e.arr = r.arr;
            v_store(g, e, coerce(g, args[i], et, "array constructor"));
        }
        r.lv = 0;
        return r;
    }
    if (t.k == GK_STRUCT) {
        if (n != t.st->n) ERR("%s constructor: %d members, %d given", t.st->name, t.st->n, n);
        struct val r = v_temp(g, t);
        int off = 0;
        for (int i = 0; i < n; i++) {
            struct val e = r; e.t = t.st->f[i].t; e.index += off; ident_swz(&e);
            v_store(g, e, coerce(g, args[i], t.st->f[i].t, "struct constructor"));
            off += ty_regs(t.st->f[i].t);
        }
        r.lv = 0;
        return r;
    }
    int all_const = 1;
    for (int i = 0; i < n; i++) if (!args[i].is_const) all_const = 0;
    if (t.cols) {
        int cols = t.cols, rows = t.vec;
        uint32_t cv[16];
        if (n == 1 && ty_is_scalar(args[0].t)) {
            struct val s = v_convert(g, args[0], GK_FLOAT);
            if (s.is_const) {
                for (int c = 0; c < cols; c++) for (int r = 0; r < rows; r++) cv[c * rows + r] = c == r ? s.cv[0] : 0;
                return const_val(g, t, cv);
            }
            struct val m = v_temp(g, t);
            for (int c = 0; c < cols; c++) {
                struct val col = v_col(g, m, c);
                v_store(g, col, v_bcast(v_fimm(g, 0), rows));
                if (c < rows) { struct val d = v_comp(col, c); d.lv = 1; v_store(g, d, s); }
            }
            m.lv = 0;
            return m;
        }
        if (n == 1 && ty_is_matrix(args[0].t)) {
            struct val a = args[0];
            struct val m = v_temp(g, t);
            for (int c = 0; c < cols; c++) {
                struct val col = v_col(g, m, c);
                for (int r = 0; r < rows; r++) {
                    struct val d = v_comp(col, r); d.lv = 1;
                    if (c < a.t.cols && r < a.t.vec) v_store(g, d, v_comp(v_col(g, a, c), r));
                    else v_store(g, d, v_fimm(g, c == r ? 1.0f : 0.0f));
                }
            }
            m.lv = 0;
            return m;
        }
        struct comps cs; cs.n = 0;
        for (int i = 0; i < n; i++) gather(g, &cs, args[i]);
        if (cs.n < cols * rows) ERR("matrix constructor: not enough components");
        if (all_const) {
            for (int i = 0; i < cols * rows; i++) cv[i] = conv1(cs.v[i].t.k, GK_FLOAT, cs.v[i].cv[0]);
            return const_val(g, t, cv);
        }
        struct val m = v_temp(g, t);
        for (int c = 0; c < cols; c++) {
            struct val col = v_col(g, m, c);
            for (int r = 0; r < rows; r++) { struct val d = v_comp(col, r); d.lv = 1; v_store(g, d, v_convert(g, cs.v[c * rows + r], GK_FLOAT)); }
        }
        m.lv = 0;
        return m;
    }
    /* scalars and vectors */
    int nc = t.vec;
    if (n == 1 && ty_is_scalar(args[0].t)) {
        struct val s = v_convert(g, args[0], t.k);
        return v_bcast(s, nc);
    }
    struct comps cs; cs.n = 0;
    for (int i = 0; i < n; i++) gather(g, &cs, args[i]);
    if (cs.n < nc) ERR("constructor: not enough components");
    if (all_const) {
        uint32_t cv[4];
        for (int i = 0; i < nc; i++) cv[i] = conv1(cs.v[i].t.k, t.k, cs.v[i].cv[0]);
        return v_imm(g, t, cv);
    }
    /* one source register used in order: a swizzle, no copy */
    if (n == 1 && !ty_is_matrix(args[0].t) && args[0].t.k == t.k) { struct val r = args[0]; r.t = t; r.lv = 0; r.is_const = 0; return r; }
    struct val r = v_temp(g, t);
    for (int i = 0; i < nc; i++) { struct val d = v_comp(r, i); d.lv = 1; v_store(g, d, v_convert(g, cs.v[i], t.k)); }
    r.lv = 0;
    return r;
}

/* ---- calls ------------------------------------------------------------------------------------- */

static struct ufunc *find_funcs(struct gen *g, const char *name, int *count)
{
    *count = 0;
    struct ufunc *first = NULL;
    for (int i = 0; i < g->nfns; i++) if (!strcmp(g->fns[i].d->name, name)) { if (!first) first = &g->fns[i]; (*count)++; }
    return first;
}

static void gen_function_body(struct gen *g, struct ufunc *f, struct val *args, struct node **argn, int nargs, struct val *ret);

static struct val call(struct gen *g, struct node *n)
{
    int line = n->line;
    /* x.length() */
    if (n->a) {
        struct val obj = expr(g, n->a);
        if (strcmp(n->name, "length") || n->n) ERR("unknown method '%s'", n->name);
        if (obj.t.array > 0) return v_iimm(g, obj.t.array);
        if (ty_is_matrix(obj.t)) return v_iimm(g, obj.t.cols);
        if (ty_is_vector(obj.t)) return v_iimm(g, ncomp(obj.t));
        ERR("length() of something that is not an array");
    }
    struct val args[32];
    if (n->n > 32) ERR("too many arguments");
    for (int i = 0; i < n->n; i++) args[i] = expr(g, n->l[i]);
    g->c->line = line;
    /* a constructor */
    int ok;
    struct gtype t;
    if (n->ts) t = resolve_tspec(g, n->ts), ok = 1;
    else { t = builtin_type(n->name, &ok); if (!ok) { struct gstruct *s = find_struct(g, n->name); if (s) { t = ty_scalar(GK_STRUCT); t.st = s; ok = 1; } } }
    if (ok) return construct(g, t, args, n->n);
    /* a user function, by overload */
    int nf;
    struct ufunc *f0 = find_funcs(g, n->name, &nf);
    if (f0) {
        struct ufunc *best = NULL;
        int best_cost = 1 << 30, ties = 0;
        for (int i = 0; i < g->nfns; i++) {
            struct ufunc *f = &g->fns[i];
            if (strcmp(f->d->name, n->name) || f->np != n->n) continue;
            int cost = 0;
            for (int k = 0; k < f->np && cost >= 0; k++) {
                if (ty_eq(args[k].t, f->pt[k])) continue;
                int is_out = f->d->params[k]->quals & Q_OUT;
                if (!is_out && implicit_ok(args[k].t, f->pt[k])) cost++;
                else cost = -1;
            }
            if (cost < 0) continue;
            if (cost < best_cost) { best = f; best_cost = cost; ties = 0; }
            else if (cost == best_cost && f->d != best->d) ties++;
        }
        if (!best) ERR("no overload of '%s' takes these arguments", n->name);
        if (ties) ERR("ambiguous call to '%s'", n->name);
        if (!best->d->body) {
            /* a prototype: find the definition */
            for (int i = 0; i < g->nfns; i++) {
                struct ufunc *f = &g->fns[i];
                if (f->d->body && !strcmp(f->d->name, n->name) && f->np == best->np) {
                    int same = 1;
                    for (int k = 0; k < f->np; k++) if (!ty_eq(f->pt[k], best->pt[k])) same = 0;
                    if (same) { best = f; break; }
                }
            }
            if (!best->d->body) ERR("'%s' is declared but never defined", n->name);
        }
        struct val ret;
        gen_function_body(g, best, args, n->l, n->n, &ret);
        return ret;
    }
    struct val out;
    if (call_builtin(g, n->name, args, n->n, &out) == 0) return out;
    ERR("'%s' undeclared", n->name);
}

/* ---- control flow ------------------------------------------------------------------------------ */

static void flow(struct gen *g, const char *op) { emit(g, op, NULL, NULL, NULL, NULL, NULL); }

static void uif(struct gen *g, struct val cond)
{
    char s[96];
    cond = v_direct(g, cond);
    src_str(g, s, sizeof s, v_comp(cond, 0), 0);
    emit(g, "UIF", NULL, s, NULL, NULL, NULL);
}

static void set_flag(struct gen *g, int reg)
{
    char d[48], s[96];
    snprintf(d, sizeof d, "TEMP[%d].x", reg);
    src_str(g, s, sizeof s, v_bimm(g, 1), 0);
    emit(g, "MOV", d, s, NULL, NULL, NULL);
}

static void clear_flag(struct gen *g, int reg)
{
    char d[48], s[96];
    snprintf(d, sizeof d, "TEMP[%d].x", reg);
    src_str(g, s, sizeof s, v_bimm(g, 0), 0);
    emit(g, "MOV", d, s, NULL, NULL, NULL);
}

static void brk_if_flag(struct gen *g, int reg)
{
    char s[48];
    snprintf(s, sizeof s, "TEMP[%d].xxxx", reg);
    emit(g, "UIF", NULL, s, NULL, NULL, NULL);
    flow(g, "BRK");
    flow(g, "ENDIF");
}

static int new_flag(struct gen *g) { struct val v = v_temp(g, ty_scalar(GK_BOOL)); clear_flag(g, v.index); return v.index; }

/* the innermost function level, or NULL (main without returns inside loops) */
static struct lvl *func_level(struct gen *g)
{
    for (int i = g->nlv - 1; i >= 0; i--) if (g->lv[i].k == LV_FUNC) return &g->lv[i];
    return NULL;
}

/* after a loop or switch ends: a return passing through goes on out */
static void after_loop(struct gen *g)
{
    struct lvl *f = func_level(g);
    if (f && f->ret_flag >= 0) brk_if_flag(g, f->ret_flag);
}

static void push_lv(struct gen *g, enum lvk k)
{
    if (g->nlv >= 64) ERR("control flow nested too deep");
    struct lvl *l = &g->lv[g->nlv++];
    memset(l, 0, sizeof *l);
    l->k = k;
    l->brk_flag = l->cont_flag = l->ret_flag = -1;
}

/* does a `continue` / `break` in n reach the loop it is in (not an inner loop's)? */
static int finds(struct node *n, enum nk what, int through_switch)
{
    if (!n) return 0;
    if (n->k == what) return 1;
    if (n->k == N_FOR || n->k == N_WHILE || n->k == N_DO) return 0;
    if (n->k == N_SWITCH && !through_switch) return 0;
    if (finds(n->a, what, through_switch) || finds(n->b, what, through_switch) || finds(n->c, what, through_switch) || finds(n->d, what, through_switch)) return 1;
    for (int i = 0; i < n->n; i++) if (finds(n->l[i], what, through_switch)) return 1;
    return 0;
}

static void do_continue(struct gen *g)
{
    int top = g->nlv - 1;
    if (top < 0) ERR("'continue' outside a loop");
    struct lvl *l = &g->lv[top];
    if (l->k == LV_SWITCH) {
        if (l->cont_flag < 0) ERR("internal: continue through a switch without a flag");
        set_flag(g, l->cont_flag);
        flow(g, "BRK");
        return;
    }
    if (l->k == LV_BODY) { flow(g, "BRK"); return; }
    ERR("'continue' outside a loop");
}

static void do_break(struct gen *g)
{
    int top = g->nlv - 1;
    if (top < 0 || g->lv[top].k == LV_FUNC) ERR("'break' outside a loop or switch");
    struct lvl *l = &g->lv[top];
    if (l->k == LV_BODY) {
        struct lvl *loop = &g->lv[top - 1];
        if (loop->brk_flag < 0) ERR("internal: break from a wrapped body without a flag");
        set_flag(g, loop->brk_flag);
    }
    flow(g, "BRK");
}

/* while/for/do: cond may be NULL; step runs after the body (and after continue) */
static void gen_loop(struct gen *g, struct node *cond, struct node *body, struct node *step, int cond_first)
{
    int has_cont = finds(body, N_CONTINUE, 1);
    int has_brk = finds(body, N_BREAK, 0);
    push_lv(g, LV_LOOP);
    struct lvl *loop = &g->lv[g->nlv - 1];
    if (has_cont && has_brk) loop->brk_flag = new_flag(g);
    int brk_flag = loop->brk_flag;
    flow(g, "BGNLOOP");
    if (cond && cond_first) {
        struct val c = expr(g, cond);
        if (!ty_is_scalar(c.t) || c.t.k != GK_BOOL) ERR("a loop condition must be a bool");
        uif(g, v_not(g, c)); flow(g, "BRK"); flow(g, "ENDIF");
    }
    if (has_cont) {
        loop->has_body = 1;
        push_lv(g, LV_BODY);
        flow(g, "BGNLOOP");
        scope_push(g);
        stmt(g, body);
        scope_pop(g);
        flow(g, "BRK");
        flow(g, "ENDLOOP");
        g->nlv--;
        after_loop(g);
        if (brk_flag >= 0) brk_if_flag(g, brk_flag);
    } else {
        scope_push(g);
        stmt(g, body);
        scope_pop(g);
    }
    if (step) expr(g, step);
    if (cond && !cond_first) {
        struct val c = expr(g, cond);
        if (!ty_is_scalar(c.t) || c.t.k != GK_BOOL) ERR("a loop condition must be a bool");
        uif(g, v_not(g, c)); flow(g, "BRK"); flow(g, "ENDIF");
    }
    flow(g, "ENDLOOP");
    g->nlv--;
    after_loop(g);
}

static void gen_switch(struct gen *g, struct node *n)
{
    struct val sel = expr(g, n->a);
    if (!ty_is_scalar(sel.t) || (sel.t.k != GK_INT && sel.t.k != GK_UINT)) ERR("switch needs an integer");
    sel = v_copy(g, sel);
    struct node *body = n->b;
    /* the case values */
    uint32_t vals[256]; int nv = 0, has_default = 0;
    for (int i = 0; i < body->n; i++) {
        struct node *s = body->l[i];
        if (s->k == N_CASE) {
            if (nv >= 256) ERR("too many cases");
            struct val cv = expr(g, s->a);
            if (!cv.is_const || !ty_is_scalar(cv.t) || (cv.t.k != GK_INT && cv.t.k != GK_UINT)) ERR("a case label must be a constant integer");
            for (int k = 0; k < nv; k++) if (vals[k] == cv.cv[0]) ERR("duplicate case %d", (int)cv.cv[0]);
            vals[nv++] = cv.cv[0];
        } else if (s->k == N_DEFAULT) {
            if (has_default) ERR("two default labels");
            has_default = 1;
        }
    }
    struct val any = v_bimm(g, 0);
    if (has_default)
        for (int k = 0; k < nv; k++)
            any = op2(g, "OR", ty_scalar(GK_BOOL), any, op2(g, "USEQ", ty_scalar(GK_BOOL), sel, v_imm(g, sel.t, &vals[k])));
    if (has_default) any = v_copy(g, any);
    int ft = new_flag(g);
    struct val ftv = reg_val(F_TEMP, ft, ty_scalar(GK_BOOL), 1);
    push_lv(g, LV_SWITCH);
    if (finds(body, N_CONTINUE, 1)) g->lv[g->nlv - 1].cont_flag = new_flag(g);
    int cont_flag = g->lv[g->nlv - 1].cont_flag;
    flow(g, "BGNLOOP");
    scope_push(g);
    int open = 0, k = 0;
    for (int i = 0; i < body->n; i++) {
        struct node *s = body->l[i];
        if (s->k == N_CASE || s->k == N_DEFAULT) {
            if (open) { flow(g, "ENDIF"); open = 0; }
            struct val m = s->k == N_CASE ? op2(g, "USEQ", ty_scalar(GK_BOOL), sel, v_imm(g, sel.t, &vals[k++])) : v_not(g, any);
            v_store(g, ftv, op2(g, "OR", ty_scalar(GK_BOOL), ftv, m));
            continue;
        }
        if (!open) { uif(g, ftv); open = 1; }
        stmt(g, s);
    }
    if (open) flow(g, "ENDIF");
    scope_pop(g);
    flow(g, "BRK");
    flow(g, "ENDLOOP");
    g->nlv--;
    after_loop(g);
    if (cont_flag >= 0) {
        char s[48];
        snprintf(s, sizeof s, "TEMP[%d].xxxx", cont_flag);
        emit(g, "UIF", NULL, s, NULL, NULL, NULL);
        do_continue(g);
        flow(g, "ENDIF");
    }
}

static void do_return(struct gen *g, struct node *n)
{
    struct lvl *f = func_level(g);
    if (n->a) {
        struct val v = expr(g, n->a);
        if (!f || !f->ret) ERR("'return' with a value in a void function");
        v_store(g, *f->ret, coerce(g, v, f->ret->t, "return"));
    } else if (f && f->ret) ERR("'return' needs a value");
    if (!f) return;                     /* a tail return: nothing follows */
    if (&g->lv[g->nlv - 1] != f) {
        if (f->ret_flag < 0) ERR("internal: return from a loop without a flag");
        set_flag(g, f->ret_flag);
    }
    flow(g, "BRK");
}

/* ---- declarations in a body -------------------------------------------------------------------- */

static void local_decl(struct gen *g, struct node *d)
{
    if (d->quals & ~(Q_CONST | Q_INVARIANT)) ERR("storage qualifiers are not allowed on local variables");
    struct gtype base = resolve_tspec(g, d->ts);
    for (int i = 0; i < d->n; i++) {
        struct node *v = d->l[i];
        g->c->line = v->line;
        struct gtype t = with_array(g, base, v);
        if (t.k == GK_VOID) ERR("'%s': variables cannot be void", v->name);
        if (t.k == GK_SAMPLER) ERR("'%s': samplers can only be uniforms or parameters", v->name);
        struct val init;
        int has_init = v->a != NULL;
        if (has_init) {
            init = expr(g, v->a);
            g->c->line = v->line;
            if (t.array < 0) { if (!init.t.array) ERR("'%s': an unsized array needs an array initializer", v->name); t.array = init.t.array; }
            init = coerce(g, init, t, "initializer");
        } else if (t.array < 0) ERR("'%s' needs an array size", v->name);
        if (d->quals & Q_CONST) {
            if (!has_init) ERR("const '%s' needs an initializer", v->name);
            if (init.is_const && !t.array && t.k != GK_STRUCT) { sym_add(g, v->name, t, init, Q_CONST); continue; }
            struct val s = v_copy(g, init);
            sym_add(g, v->name, t, s, Q_CONST);
            continue;
        }
        struct val s = v_temp(g, t);
        if (has_init) v_store(g, s, init);
        sym_add(g, v->name, t, s, 0);
    }
}

/* ---- statements ------------------------------------------------------------------------------ */

static void stmt(struct gen *g, struct node *n)
{
    if (!n) return;
    g->c->line = n->line;
    switch (n->k) {
    case N_EMPTY: return;
    case N_BLOCK:
        scope_push(g);
        for (int i = 0; i < n->n; i++) stmt(g, n->l[i]);
        scope_pop(g);
        return;
    case N_DECL: local_decl(g, n); return;
    case N_EXPR: expr(g, n->a); return;
    case N_IF: {
        struct val c = expr(g, n->a);
        if (!ty_is_scalar(c.t) || c.t.k != GK_BOOL) ERR("an if condition must be a bool");
        if (c.is_const) {
            scope_push(g);
            stmt(g, c.cv[0] ? n->b : n->c);
            scope_pop(g);
            return;
        }
        uif(g, c);
        scope_push(g); stmt(g, n->b); scope_pop(g);
        if (n->c) { flow(g, "ELSE"); scope_push(g); stmt(g, n->c); scope_pop(g); }
        flow(g, "ENDIF");
        return;
    }
    case N_FOR:
        scope_push(g);
        if (n->a) stmt(g, n->a);
        gen_loop(g, n->b, n->d, n->c, 1);
        scope_pop(g);
        return;
    case N_WHILE: gen_loop(g, n->a, n->b, NULL, 1); return;
    case N_DO: gen_loop(g, n->a, n->b, NULL, 0); return;
    case N_SWITCH: gen_switch(g, n); return;
    case N_CASE: case N_DEFAULT: ERR("case label outside a switch");
    case N_BREAK: do_break(g); return;
    case N_CONTINUE: do_continue(g); return;
    case N_RETURN: do_return(g, n); return;
    case N_DISCARD:
        if (g->c->stage != GL_FRAGMENT_SHADER) ERR("discard in a vertex shader");
        flow(g, "KILL");
        return;
    default: ERR("not a statement");
    }
}

/* ---- functions ------------------------------------------------------------------------------- */

/* how many `return`s; whether one sits inside a loop or switch */
static void count_returns(struct node *n, int depth, int *count, int *in_loop)
{
    if (!n) return;
    if (n->k == N_RETURN) { (*count)++; if (depth) *in_loop = 1; return; }
    int d = depth + (n->k == N_FOR || n->k == N_WHILE || n->k == N_DO || n->k == N_SWITCH);
    count_returns(n->a, d, count, in_loop); count_returns(n->b, d, count, in_loop);
    count_returns(n->c, d, count, in_loop); count_returns(n->d, d, count, in_loop);
    for (int i = 0; i < n->n; i++) count_returns(n->l[i], d, count, in_loop);
}

/* the body of f (or main) at this point: parameters bound, returns lowered */
static void gen_function_body(struct gen *g, struct ufunc *f, struct val *args, struct node **argn, int nargs, struct val *ret)
{
    (void)argn;
    struct fdecl *d = f->d;
    if (f->inlining) ERR("'%s' is recursive (not allowed in GLSL)", d->name);
    f->inlining = 1;
    int saved_frame = g->frame, saved_scope = g->scope;
    g->frame = ++g->nframes;
    g->scope = 1;
    g->in_func++;
    struct val retv;
    memset(&retv, 0, sizeof retv);
    if (f->ret.k != GK_VOID) retv = v_temp(g, f->ret);
    /* parameters */
    struct sym *outs[32]; int out_arg[32]; int nouts = 0;
    for (int i = 0; i < d->nparams && i < nargs; i++) {
        struct node *p = d->params[i];
        struct gtype pt = f->pt[i];
        struct val local;
        if (pt.k == GK_SAMPLER) {
            if (args[i].file != F_SAMP) ERR("'%s': argument %d is not a sampler", d->name, i + 1);
            local = args[i];
        } else if ((p->quals & Q_OUT) && !(p->quals & Q_IN)) {
            local = v_temp(g, pt);
        } else {
            local = v_copy(g, coerce(g, args[i], pt, "argument"));
            local.lv = 1; local.is_const = 0;
        }
        if (p->quals & Q_OUT) {
            if (!is_lvalue(args[i])) ERR("'%s': out argument %d is not a variable", d->name, i + 1);
            if (nouts < 32) out_arg[nouts] = i;
        }
        struct sym *s = sym_add(g, p->name ? p->name : "", pt, local, p->quals & Q_CONST);
        if (p->quals & Q_OUT) outs[nouts++] = s;
    }
    /* returns other than a final one become breaks out of a one-pass loop */
    int count = 0, in_loop = 0;
    count_returns(d->body, 0, &count, &in_loop);
    struct node *last = d->body->n ? d->body->l[d->body->n - 1] : NULL;
    int wrap = count > 1 || (count == 1 && !(last && last->k == N_RETURN)) || in_loop;
    if (wrap) {
        push_lv(g, LV_FUNC);
        struct lvl *l = &g->lv[g->nlv - 1];
        l->ret = f->ret.k != GK_VOID ? &retv : NULL;
        if (in_loop) l->ret_flag = new_flag(g);
        flow(g, "BGNLOOP");
        stmt(g, d->body);
        flow(g, "BRK");
        flow(g, "ENDLOOP");
        g->nlv--;
    } else {
        /* no lowering: a final return writes the result */
        push_lv(g, LV_FUNC);
        struct lvl *l = &g->lv[g->nlv - 1];
        l->ret = f->ret.k != GK_VOID ? &retv : NULL;
        int saved = g->nlv;
        scope_push(g);
        for (int i = 0; i < d->body->n; i++) {
            struct node *s = d->body->l[i];
            if (s->k == N_RETURN) {
                if (s->a) { struct val v = expr(g, s->a); if (!l->ret) ERR("'return' with a value in a void function"); v_store(g, retv, coerce(g, v, f->ret, "return")); }
                else if (l->ret) ERR("'return' needs a value");
            } else stmt(g, s);
        }
        scope_pop(g);
        g->nlv = saved - 1;
    }
    for (int i = 0; i < nouts; i++) {
        struct val lv = outs[i]->v;
        lv.t = outs[i]->t;
        v_store(g, args[out_arg[i]], coerce(g, v_direct(g, lv), args[out_arg[i]].t, "out argument"));
    }
    /* the callee's names go */
    while (g->nsyms > 0 && g->syms[g->nsyms - 1].frame == g->frame) g->nsyms--;
    g->in_func--;
    g->frame = saved_frame;
    g->scope = saved_scope;
    f->inlining = 0;
    retv.lv = 0;
    *ret = retv;
}

static void declare_function(struct gen *g, struct fdecl *d)
{
    g->c->line = d->line;
    struct gtype ret = resolve_tspec(g, d->ret);
    if (ret.array < 0) ERR("a returned array needs a size");
    struct gtype *pt = ar_alloc(g->c, (size_t)(d->nparams ? d->nparams : 1) * sizeof *pt);
    for (int i = 0; i < d->nparams; i++) {
        pt[i] = with_array(g, resolve_tspec(g, d->params[i]->ts), d->params[i]);
        if (pt[i].array < 0) ERR("parameter arrays need a size");
        if (pt[i].k == GK_VOID) ERR("a parameter cannot be void");
    }
    if (is_builtin_func(d->name) && strcmp(d->name, "main")) {
        /* user overloads of built-in names hide them: allowed */
    }
    /* a definition after its prototype: replace the prototype */
    for (int i = 0; i < g->nfns; i++) {
        struct ufunc *f = &g->fns[i];
        if (strcmp(f->d->name, d->name) || f->np != d->nparams) continue;
        int same = 1;
        for (int k = 0; k < f->np; k++) if (!ty_eq(f->pt[k], pt[k])) same = 0;
        if (!same) continue;
        if (!ty_eq(f->ret, ret)) ERR("'%s' redeclared with a different return type", d->name);
        if (f->d->body && d->body) ERR("'%s' is defined twice", d->name);
        if (d->body) f->d = d;
        return;
    }
    if (g->nfns == g->capfns) {
        int cap = g->capfns ? g->capfns * 2 : 32;
        struct ufunc *n = ar_alloc(g->c, (size_t)cap * sizeof *n);
        if (g->nfns) memcpy(n, g->fns, (size_t)g->nfns * sizeof *n);
        g->fns = n; g->capfns = cap;
    }
    struct ufunc *f = &g->fns[g->nfns++];
    memset(f, 0, sizeof *f);
    f->d = d; f->ret = ret; f->pt = pt; f->np = d->nparams;
}

/* ---- global declarations ------------------------------------------------------------------------ */

static void global_var(struct gen *g, struct node *d)
{
    struct glc *c = g->c;
    struct gtype base = resolve_tspec(g, d->ts);
    int q = d->quals;
    int vs = c->stage == GL_VERTEX_SHADER;
    int modern = c->version >= 130 || c->version == 300;
    for (int i = 0; i < d->n; i++) {
        struct node *v = d->l[i];
        c->line = v->line;
        struct gtype t = with_array(g, base, v);
        if (t.k == GK_VOID) ERR("'%s': variables cannot be void", v->name);
        struct val storage;
        if (q & Q_UNIFORM) {
            if (t.array < 0) ERR("'%s': uniform arrays need a size", v->name);
            storage = declare_uniform(g, v->name, t, d->binding);
            sym_add(g, v->name, t, storage, Q_UNIFORM);
            continue;
        }
        if (t.k == GK_SAMPLER) ERR("'%s': a sampler must be a uniform", v->name);
        if (q & Q_ATTRIBUTE) {
            if (!vs) ERR("'%s': attribute in a fragment shader", v->name);
            sym_add(g, v->name, t, declare_attribute(g, v->name, t, d->location, -1), Q_CONST);
            continue;
        }
        if (q & Q_VARYING) {
            if (t.array < 0) ERR("'%s': varying arrays need a size", v->name);
            sym_add(g, v->name, t, declare_varying(g, v->name, t, q), vs ? 0 : Q_CONST);
            continue;
        }
        if ((q & Q_IN) && !(q & Q_OUT)) {
            if (!modern) ERR("'in' variables need GLSL 1.30 (use attribute/varying)");
            if (t.array < 0) ERR("'%s': input arrays need a size", v->name);
            if (vs) sym_add(g, v->name, t, declare_attribute(g, v->name, t, d->location, -1), Q_CONST);
            else sym_add(g, v->name, t, declare_varying(g, v->name, t, q), Q_CONST);
            continue;
        }
        if (q & Q_OUT) {
            if (!modern) ERR("'out' variables need GLSL 1.30 (use varying/gl_FragColor)");
            if (t.array < 0) ERR("'%s': output arrays need a size", v->name);
            if (vs) sym_add(g, v->name, t, declare_varying(g, v->name, t, q), 0);
            else sym_add(g, v->name, t, declare_frag_output(g, v->name, t, d->location, d->index), 0);
            continue;
        }
        /* const and plain globals: initializers run before main's body */
        struct val init;
        int has_init = v->a != NULL;
        if (has_init) {
            init = expr(g, v->a);
            c->line = v->line;
            if (t.array < 0) { if (!init.t.array) ERR("'%s': an unsized array needs an array initializer", v->name); t.array = init.t.array; }
            init = coerce(g, init, t, "initializer");
        } else if (t.array < 0) ERR("'%s' needs an array size", v->name);
        if (q & Q_CONST) {
            if (!has_init) ERR("const '%s' needs an initializer", v->name);
            if (init.is_const && !t.array && t.k != GK_STRUCT) { sym_add(g, v->name, t, init, Q_CONST); continue; }
            sym_add(g, v->name, t, v_copy(g, init), Q_CONST);
            continue;
        }
        struct val s = v_temp(g, t);
        if (has_init) v_store(g, s, init);
        sym_add(g, v->name, t, s, 0);
    }
}

void gen_unit(struct glc *c)
{
    struct gen *g = c->g;
    g->c = c;
    light_struct = material_struct = lightmodel_struct = NULL;
    struct zgl_compiled *o = c->out;
    /* the outputs every stage has */
    if (c->stage == GL_VERTEX_SHADER) {
        int idx = g->nout++;
        io_decl(g, 0, idx, 1, "POSITION", -1, NULL);
        struct gtype v4 = ty_vec(GK_FLOAT, 4);
        sym_add(g, "gl_Position", v4, reg_val(F_OUT, idx, v4, 1), 0);
    }
    (void)o;
    /* declarations in order; functions are registered, main is generated last */
    struct fdecl *main_fn = NULL;
    for (int i = 0; i < c->ndecls; i++) {
        struct gdecl *d = &c->decls[i];
        switch (d->k) {
        case G_PRECISION: case G_DEFAULT_LAYOUT: break;
        case G_STRUCT: c->line = 0; declare_struct(g, d->ts); break;
        case G_VAR: global_var(g, d->var); break;
        case G_BLOCK:
            if (d->quals & Q_UNIFORM) {
                /* a uniform block, for now as ordinary uniforms named Block.member */
                for (int k = 0; k < d->ts->nfields; k++) {
                    struct node *m = d->ts->fields[k];
                    struct gtype ft = resolve_tspec(g, m->ts);
                    for (int j = 0; j < m->n; j++) {
                        struct gtype t = with_array(g, ft, m->l[j]);
                        char nm[96];
                        snprintf(nm, sizeof nm, "%s.%s", d->block_name, m->l[j]->name);
                        struct val v = declare_uniform(g, ar_strdup(c, nm), t, -1);
                        if (d->instance) {
                            /* instance.member: register the member under a mangled name the field lookup finds */
                            char in[96];
                            snprintf(in, sizeof in, "%s.%s", d->instance, m->l[j]->name);
                            sym_add(g, ar_strdup(c, in), t, v, Q_UNIFORM);
                        } else sym_add(g, m->l[j]->name, t, v, Q_UNIFORM);
                    }
                }
            } else ERR("in/out interface blocks are not supported");
            break;
        case G_FUNC:
            declare_function(g, d->fn);
            if (!strcmp(d->fn->name, "main")) {
                if (d->fn->nparams) ERR("main() takes no parameters");
                if (d->fn->body) main_fn = d->fn;
            }
            break;
        }
    }
    if (!main_fn) { c->line = 0; ERR("no main() function"); }
    for (int i = 0; i < g->nfns; i++)
        if (g->fns[i].d == main_fn) {
            struct val ret;
            gen_function_body(g, &g->fns[i], NULL, NULL, 0, &ret);
        }
    flow(g, "END");
}

/* ---- the entry point ----------------------------------------------------------------------------- */

static void outs(char **buf, size_t *len, size_t *cap, const char *s)
{
    size_t n = strlen(s);
    if (*len + n + 1 > *cap) {
        size_t c2 = *cap ? *cap * 2 : 16384;
        while (c2 < *len + n + 1) c2 *= 2;
        *buf = realloc(*buf, c2);
        *cap = c2;
    }
    memcpy(*buf + *len, s, n + 1);
    *len += n;
}

static void assemble(struct glc *c, struct gen *g)
{
    char *o = NULL; size_t len = 0, cap = 0;
    char line[160];
    outs(&o, &len, &cap, c->stage == GL_VERTEX_SHADER ? "VERT\n" : "FRAG\n");
    if (g->writes_fragcolor) outs(&o, &len, &cap, "PROPERTY FS_COLOR0_WRITES_ALL_CBUFS 1\n");
    for (int i = 0; i < g->nin; i++) if (g->in_decl[i][0]) outs(&o, &len, &cap, g->in_decl[i]);
    for (int i = 0; i < g->nout; i++) if (g->out_decl[i][0]) outs(&o, &len, &cap, g->out_decl[i]);
    for (int i = 0; i < g->nsv; i++) outs(&o, &len, &cap, g->sv_decl[i]);
    if (g->nconst) { snprintf(line, sizeof line, "DCL CONST[0..%d]\n", g->nconst - 1); outs(&o, &len, &cap, line); }
    for (int i = 0; i < g->nsamp; i++) {
        enum zgl_type st = g->samp_type[i];
        enum zgl_type b = zgl_type_base(st);
        snprintf(line, sizeof line, "DCL SAMP[%d]\nDCL SVIEW[%d], %s, %s\n", i, i, zgl_type_tgsi_target(st),
                 b == T_INT ? "SINT" : b == T_UINT ? "UINT" : "FLOAT");
        outs(&o, &len, &cap, line);
    }
    /* temps: plain runs between the array ranges */
    int t = 0;
    for (int a = 0; a <= g->narrays; a++) {
        int end = a < g->narrays ? g->arrays[a].first : g->ntemps;
        if (end > t) { snprintf(line, sizeof line, "DCL TEMP[%d..%d]\n", t, end - 1); outs(&o, &len, &cap, line); }
        if (a < g->narrays) {
            snprintf(line, sizeof line, "DCL TEMP[%d..%d], ARRAY(%d)\n", g->arrays[a].first, g->arrays[a].first + g->arrays[a].n - 1, a + 1);
            outs(&o, &len, &cap, line);
            t = g->arrays[a].first + g->arrays[a].n;
        }
    }
    if (g->uses_addr) outs(&o, &len, &cap, "DCL ADDR[0]\n");
    for (int i = 0; i < g->nimm; i++) {
        uint32_t *v = g->imm[i].v;
        float f[4];
        memcpy(f, v, 16);
        if (g->imm[i].k == 'F' && isfinite(f[0]) && isfinite(f[1]) && isfinite(f[2]) && isfinite(f[3]))
            snprintf(line, sizeof line, "IMM FLT32 {%.9g, %.9g, %.9g, %.9g}\n", f[0], f[1], f[2], f[3]);
        else if (g->imm[i].k == 'F')        /* inf and NaN have no decimal spelling: the bits */
            snprintf(line, sizeof line, "IMM UINT32 {%u, %u, %u, %u}\n", v[0], v[1], v[2], v[3]); else if (g->imm[i].k == 'I')
            snprintf(line, sizeof line, "IMM INT32 {%d, %d, %d, %d}\n", (int32_t)v[0], (int32_t)v[1], (int32_t)v[2], (int32_t)v[3]);
        else
            snprintf(line, sizeof line, "IMM UINT32 {%u, %u, %u, %u}\n", v[0], v[1], v[2], v[3]);
        outs(&o, &len, &cap, line);
    }
    outs(&o, &len, &cap, g->code ? g->code : "");
    c->out->tgsi = o;
    c->out->nconsts = g->nconst;
    c->out->version = c->version;
}

int zgl_compile_linked(const char *source, GLenum type, const struct zgl_compiled *vs, struct zgl_compiled *out, char *log, size_t logsize)
{
    struct glc *c = calloc(1, sizeof *c);
    struct gen *g = calloc(1, sizeof *g);
    if (!c || !g) { free(c); free(g); snprintf(log, logsize, "out of memory\n"); return -1; }
    memset(out, 0, sizeof *out);
    c->stage = type; c->out = out; c->vs = vs; c->log = log; c->logsize = logsize; c->g = g;
    log[0] = 0;
    int rc = 0;
    if (setjmp(c->err)) {
        rc = -1;
        free(out->tgsi);
        out->tgsi = NULL;
    } else {
        pp_run(c, source);
        parse_unit(c);
        gen_unit(c);
        assemble(c, g);
    }
    free(g->code);
    while (c->arena) { struct arena_blk *n = c->arena->next; free(c->arena); c->arena = n; }
    free(g);
    free(c);
    return rc;
}

int zgl_compile(const char *source, GLenum type, struct zgl_compiled *out, char *log, size_t logsize)
{
    return zgl_compile_linked(source, type, NULL, out, log, logsize);
}

void zgl_compiled_free(struct zgl_compiled *c)
{
    free(c->tgsi);
    c->tgsi = NULL;
}
