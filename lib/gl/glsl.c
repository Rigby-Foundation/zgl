/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/*
 * GLSL 1.20 to TGSI text, for virglrenderer.
 *
 * A single-pass compiler: a recursive-descent parser that emits TGSI as it
 * goes. Values live in TGSI registers (TEMP, IN, OUT, CONST, IMM) with a
 * swizzle; every subexpression gets a fresh temporary (the host's GLSL
 * compiler cleans that up). Types are float, int, bool and their vectors,
 * mat2/3/4, sampler2D; ints and bools are computed in floats (1.0/0.0),
 * which is what the fixed-function era shaders this targets expect.
 *
 * Structure: `for` loops with constant bounds are unrolled at parse time
 * (the loop variable becomes a compile-time constant, so array indexing by
 * it is static); other loops become BGNLOOP/ENDLOOP. User functions are
 * inlined at each call. Varyings get their GENERIC index from a table the
 * linker shares between the vertex and fragment shader.
 */
#include "zgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <setjmp.h>

/* ---- lexer ------------------------------------------------------------------ */

enum tok {
    TK_EOF, TK_IDENT, TK_FLOAT, TK_INT, TK_PUNCT,
};

struct token { enum tok kind; char text[64]; double num; int line; };

struct cc;                                   /* the compiler state, below */

struct define { char name[48]; char value[128]; };

struct lexer {
    const char *src, *p;
    int line;
    struct token cur, ahead;
    int have_ahead;
    struct define defs[32]; int ndefs;
    const char *expand;                      /* text of a define being expanded */
    const char *expand_resume;
};

static void lex_init(struct lexer *l, const char *src) { memset(l, 0, sizeof *l); l->src = l->p = src; l->line = 1; }

static const char *puncts[] = {
    "<<=", ">>=", "++", "--", "<=", ">=", "==", "!=", "&&", "||", "^^", "+=", "-=", "*=", "/=",
    "(", ")", "[", "]", "{", "}", ".", ",", ";", "+", "-", "*", "/", "<", ">", "!", "?", ":", "=", "%", "&", "|", "^", "~", NULL
};

static int is_ident_start(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_ident(int c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }

static void skip_ws(struct lexer *l)
{
    for (;;) {
        if (l->expand && !*l->p) { l->p = l->expand_resume; l->expand = NULL; continue; }
        if (*l->p == '\n') { l->line++; l->p++; continue; }
        if (*l->p == ' ' || *l->p == '\t' || *l->p == '\r') { l->p++; continue; }
        if (l->p[0] == '/' && l->p[1] == '/') { while (*l->p && *l->p != '\n') l->p++; continue; }
        if (l->p[0] == '/' && l->p[1] == '*') { l->p += 2; while (*l->p && !(l->p[0] == '*' && l->p[1] == '/')) { if (*l->p == '\n') l->line++; l->p++; } if (*l->p) l->p += 2; continue; }
        if (*l->p == '#' && !l->expand) {
            /* preprocessor: #version/#extension/#pragma are ignored, #define of a
             * constant is remembered, everything else is skipped */
            const char *e = l->p + 1;
            while (*e == ' ' || *e == '\t') e++;
            if (strncmp(e, "define", 6) == 0 && !is_ident(e[6])) {
                e += 6;
                while (*e == ' ' || *e == '\t') e++;
                struct define *d = l->ndefs < 32 ? &l->defs[l->ndefs] : NULL;
                size_t n = 0;
                while (is_ident(*e) && n < 47) { if (d) d->name[n] = *e; n++; e++; }
                if (d) d->name[n] = 0;
                while (*e == ' ' || *e == '\t') e++;
                n = 0;
                while (*e && *e != '\n' && n < 127) { if (d) d->value[n] = *e; n++; e++; }
                if (d) { d->value[n] = 0; l->ndefs++; }
            }
            while (*l->p && *l->p != '\n') l->p++;
            continue;
        }
        break;
    }
}

static void lex_next_raw(struct lexer *l, struct token *t)
{
    skip_ws(l);
    t->line = l->line;
    const char *p = l->p;
    if (!*p) { t->kind = TK_EOF; t->text[0] = 0; return; }
    if (is_ident_start(*p)) {
        size_t n = 0;
        while (is_ident(*p) && n < 63) t->text[n++] = *p++;
        t->text[n] = 0;
        l->p = p;
        t->kind = TK_IDENT;
        /* a #define'd name expands in place */
        if (!l->expand)
            for (int i = 0; i < l->ndefs; i++)
                if (strcmp(l->defs[i].name, t->text) == 0) {
                    l->expand_resume = l->p;
                    l->expand = l->defs[i].value;
                    l->p = l->expand;
                    lex_next_raw(l, t);
                    return;
                }
        return;
    }
    if ((*p >= '0' && *p <= '9') || (*p == '.' && p[1] >= '0' && p[1] <= '9')) {
        char *end;
        double v = strtod(p, &end);
        int is_float = 0;
        for (const char *q = p; q < end; q++) if (*q == '.' || *q == 'e' || *q == 'E') is_float = 1;
        if (*end == 'f' || *end == 'F') { end++; is_float = 1; }
        if (*end == 'u' || *end == 'U') end++;
        t->kind = is_float ? TK_FLOAT : TK_INT;
        t->num = v;
        size_t n = (size_t)(end - p) < 63 ? (size_t)(end - p) : 63;
        memcpy(t->text, p, n); t->text[n] = 0;
        l->p = end;
        return;
    }
    for (int i = 0; puncts[i]; i++) {
        size_t n = strlen(puncts[i]);
        if (strncmp(p, puncts[i], n) == 0) {
            t->kind = TK_PUNCT;
            memcpy(t->text, puncts[i], n + 1);
            l->p = p + n;
            return;
        }
    }
    t->kind = TK_PUNCT; t->text[0] = *p; t->text[1] = 0; l->p = p + 1;
}

/* ---- compiler state ---------------------------------------------------------- */

enum file { F_TEMP, F_IN, F_OUT, F_CONST, F_IMM, F_SAMP };

struct val {
    enum zgl_type type;
    enum file file;
    int index;                  /* register (matrices: the first of N consecutive) */
    char swz[4];                /* component map for vectors/scalars */
    int neg;
    int is_const;               /* a compile-time constant scalar/vector: value in c[] */
    float c[4];
    int lvalue;                 /* can be assigned: file/index/swz name the storage */
};

struct sym {
    char name[48];
    enum zgl_type type;
    int array;                  /* element count or 0 */
    enum file file;
    int index;                  /* base register */
    int is_const_int;           /* an unrolled loop counter: value in ival */
    int ival;
    int scope;
    int builtin;
};

struct func {
    char name[48];
    const char *body;           /* source position of the parameter list */
    enum zgl_type ret;
    int line;
};

struct outbuf { char *s; size_t len, cap; };

struct cc {
    struct lexer lx;
    struct token tok;           /* current token */
    GLenum stage;
    struct outbuf code;         /* instructions */
    struct outbuf decl;         /* declarations */
    int ninst;
    int ntemps;
    float imm[256][4]; int nimm;
    struct sym syms[512]; int nsyms;
    int scope;
    struct func funcs[64]; int nfuncs;
    struct zgl_compiled *out;
    const struct zgl_compiled *vs;          /* when compiling a FS: the VS's varyings */
    int nin, nout;              /* IN/OUT registers used */
    int in_decl[64]; char in_decl_txt[64][96]; int nin_decl;
    int out_decl_used[64];
    char out_decl_txt[64][96];
    int nconst;
    int nsamp;
    char *log; size_t logsize;
    jmp_buf err;
    int loop_depth;
    int inlining;               /* > 0 while inlining a function body */
    int ret_scope;              /* the scope of the inlined function's body */
    struct val *ret_val;        /* the inlined function's result register */
    int had_return;
    int frontfacing_in;         /* FS: IN index of FACE, -1 if unused */
    int fragcoord_in;
    int pointsize_out;
};

static void ob_put(struct outbuf *b, const char *s)
{
    size_t n = strlen(s);
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->len + n + 1) cap *= 2;
        b->s = realloc(b->s, cap);
        b->cap = cap;
    }
    memcpy(b->s + b->len, s, n + 1);
    b->len += n;
}

static void obf(struct outbuf *b, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    ob_put(b, tmp);
}

static void __attribute__((noreturn)) fail(struct cc *c, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    snprintf(c->log, c->logsize, "%d: error: %s\n", c->tok.line, tmp);
    longjmp(c->err, 1);
}

static void next(struct cc *c)
{
    if (c->lx.have_ahead) { c->tok = c->lx.ahead; c->lx.have_ahead = 0; return; }
    lex_next_raw(&c->lx, &c->tok);
}

static const struct token *peek(struct cc *c)
{
    if (!c->lx.have_ahead) { lex_next_raw(&c->lx, &c->lx.ahead); c->lx.have_ahead = 1; }
    return &c->lx.ahead;
}

static int is(struct cc *c, const char *s) { return (c->tok.kind == TK_PUNCT || c->tok.kind == TK_IDENT) && strcmp(c->tok.text, s) == 0; }
static int accept(struct cc *c, const char *s) { if (is(c, s)) { next(c); return 1; } return 0; }
static void expect(struct cc *c, const char *s) { if (!accept(c, s)) fail(c, "expected '%s' before '%s'", s, c->tok.text); }

/* ---- types ---------------------------------------------------------------- */

static int vec_size(enum zgl_type t)
{
    switch (t) {
    case T_FLOAT: case T_INT: case T_BOOL: return 1;
    case T_VEC2: case T_IVEC2: case T_BVEC2: return 2;
    case T_VEC3: case T_IVEC3: case T_BVEC3: return 3;
    case T_VEC4: case T_IVEC4: case T_BVEC4: return 4;
    case T_MAT2: return 2; case T_MAT3: return 3; case T_MAT4: return 4;
    default: return 0;
    }
}
static int is_matrix(enum zgl_type t) { return t == T_MAT2 || t == T_MAT3 || t == T_MAT4; }
static int is_sampler(enum zgl_type t) { return t == T_SAMPLER2D || t == T_SAMPLERCUBE; }
static int regs_of(enum zgl_type t) { return is_matrix(t) ? vec_size(t) : 1; }
static enum zgl_type vec_of(enum zgl_type base, int n)
{
    if (base == T_INT || base == T_IVEC2 || base == T_IVEC3 || base == T_IVEC4) return n == 1 ? T_INT : (enum zgl_type)(T_IVEC2 + n - 2);
    if (base == T_BOOL || base == T_BVEC2 || base == T_BVEC3 || base == T_BVEC4) return n == 1 ? T_BOOL : (enum zgl_type)(T_BVEC2 + n - 2);
    return n == 1 ? T_FLOAT : (enum zgl_type)(T_VEC2 + n - 2);
}
static enum zgl_type base_of(enum zgl_type t) { return vec_of(t, 1); }

static enum zgl_type parse_type_name(const char *s)
{
    static const struct { const char *n; enum zgl_type t; } types[] = {
        { "void", T_VOID }, { "float", T_FLOAT }, { "int", T_INT }, { "bool", T_BOOL },
        { "vec2", T_VEC2 }, { "vec3", T_VEC3 }, { "vec4", T_VEC4 },
        { "ivec2", T_IVEC2 }, { "ivec3", T_IVEC3 }, { "ivec4", T_IVEC4 },
        { "bvec2", T_BVEC2 }, { "bvec3", T_BVEC3 }, { "bvec4", T_BVEC4 },
        { "mat2", T_MAT2 }, { "mat3", T_MAT3 }, { "mat4", T_MAT4 },
        { "sampler2D", T_SAMPLER2D }, { "samplerCube", T_SAMPLERCUBE },
    };
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++)
        if (strcmp(types[i].n, s) == 0) return types[i].t;
    return T_STRUCT;                        /* "not a type" */
}

static const char *type_name(enum zgl_type t)
{
    static const char *n[] = { "void", "float", "int", "bool", "vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4",
                               "bvec2", "bvec3", "bvec4", "mat2", "mat3", "mat4", "sampler2D", "samplerCube", "struct" };
    return n[t];
}

/* ---- registers, immediates, symbols ----------------------------------------- */

static struct val temp(struct cc *c, enum zgl_type t)
{
    struct val v;
    memset(&v, 0, sizeof v);
    v.type = t; v.file = F_TEMP; v.index = c->ntemps; c->ntemps += regs_of(t);
    v.swz[0] = 0; v.swz[1] = 1; v.swz[2] = 2; v.swz[3] = 3;
    v.lvalue = 1;
    return v;
}

static struct val imm(struct cc *c, enum zgl_type t, float x, float y, float z, float w)
{
    float f[4] = { x, y, z, w };
    int n = vec_size(t);
    if (n == 1) f[1] = f[2] = f[3] = x;
    int idx = -1;
    for (int i = 0; i < c->nimm; i++)
        if (memcmp(c->imm[i], f, sizeof f) == 0) { idx = i; break; }
    if (idx < 0) {
        if (c->nimm >= 256) fail(c, "too many constants");
        idx = c->nimm++;
        memcpy(c->imm[idx], f, sizeof f);
    }
    struct val v;
    memset(&v, 0, sizeof v);
    v.type = t; v.file = F_IMM; v.index = idx;
    v.swz[0] = 0; v.swz[1] = 1; v.swz[2] = 2; v.swz[3] = 3;
    v.is_const = 1;
    memcpy(v.c, f, sizeof f);
    return v;
}

static struct val scalar_imm(struct cc *c, float x) { return imm(c, T_FLOAT, x, x, x, x); }

static struct sym *sym_find(struct cc *c, const char *name)
{
    for (int i = c->nsyms - 1; i >= 0; i--)
        if (strcmp(c->syms[i].name, name) == 0) return &c->syms[i];
    return NULL;
}

static struct sym *sym_add(struct cc *c, const char *name, enum zgl_type t, int array, enum file file, int index)
{
    if (c->nsyms >= 512) fail(c, "too many variables");
    struct sym *s = &c->syms[c->nsyms++];
    memset(s, 0, sizeof *s);
    strncpy(s->name, name, sizeof s->name - 1);
    s->type = t; s->array = array; s->file = file; s->index = index; s->scope = c->scope;
    return s;
}

static void scope_push(struct cc *c) { c->scope++; }
static void scope_pop(struct cc *c)
{
    while (c->nsyms > 0 && c->syms[c->nsyms - 1].scope == c->scope) c->nsyms--;
    c->scope--;
}

static struct val sym_val(struct cc *c, struct sym *s)
{
    struct val v;
    memset(&v, 0, sizeof v);
    if (s->is_const_int) return scalar_imm(c, (float)s->ival);
    v.type = s->type; v.file = s->file; v.index = s->index;
    v.swz[0] = 0; v.swz[1] = 1; v.swz[2] = 2; v.swz[3] = 3;
    v.lvalue = s->file == F_TEMP || s->file == F_OUT;
    if (s->file == F_IMM) { v.is_const = 1; memcpy(v.c, c->imm[s->index], sizeof v.c); }
    return v;
}

/* ---- emission ------------------------------------------------------------------ */

static const char comp_names[] = "xyzw";

static const char *file_name(enum file f)
{
    switch (f) { case F_TEMP: return "TEMP"; case F_IN: return "IN"; case F_OUT: return "OUT"; case F_CONST: return "CONST"; case F_IMM: return "IMM"; default: return "SAMP"; }
}

/* the source operand string of v (with register offset `reg` for matrix columns) */
static void src_str(char *buf, size_t n, const struct val *v, int reg)
{
    int size = is_matrix(v->type) ? vec_size(v->type) : vec_size(v->type);
    char swz[5] = { 0 };
    if (size == 1) { swz[0] = swz[1] = swz[2] = swz[3] = comp_names[(int)v->swz[0]]; swz[4] = 0; }
    else { for (int i = 0; i < 4; i++) swz[i] = comp_names[(int)v->swz[i < size ? i : size - 1]]; swz[4] = 0; }
    snprintf(buf, n, "%s%s[%d].%s", v->neg ? "-" : "", file_name(v->file), v->index + reg, swz);
}

static void emit_inst(struct cc *c, const char *op, const char *dst, const char *a, const char *b, const char *d)
{
    obf(&c->code, "%3d: %s %s", c->ninst++, op, dst);
    if (a) obf(&c->code, ", %s", a);
    if (b) obf(&c->code, ", %s", b);
    if (d) obf(&c->code, ", %s", d);
    ob_put(&c->code, "\n");
}

static void emit_flow(struct cc *c, const char *op, const char *arg)
{
    if (arg) obf(&c->code, "%3d: %s %s\n", c->ninst++, op, arg);
    else obf(&c->code, "%3d: %s\n", c->ninst++, op);
}

/* dst string: register plus write mask for the first n components */
static void dst_str(char *buf, size_t n, const struct val *v, int reg, int ncomp)
{
    char mask[5] = { 0 };
    for (int i = 0; i < ncomp && i < 4; i++) mask[i] = comp_names[(int)v->swz[i]];
    snprintf(buf, n, "%s[%d].%s", file_name(v->file), v->index + reg, mask);
}

/* Emit op dst, a[, b[, d]] on vectors; scalars broadcast. Returns dst. */
static struct val emit3(struct cc *c, const char *op, enum zgl_type rt, const struct val *a, const struct val *b, const struct val *d)
{
    struct val r = temp(c, rt);
    char ds[64], as[64], bs[64], dd[64];
    dst_str(ds, sizeof ds, &r, 0, vec_size(rt));
    src_str(as, sizeof as, a, 0);
    if (b) src_str(bs, sizeof bs, b, 0);
    if (d) src_str(dd, sizeof dd, d, 0);
    emit_inst(c, op, ds, as, b ? bs : NULL, d ? dd : NULL);
    return r;
}

static struct val emit2(struct cc *c, const char *op, enum zgl_type rt, const struct val *a, const struct val *b) { return emit3(c, op, rt, a, b, NULL); }
static struct val emit1(struct cc *c, const char *op, enum zgl_type rt, const struct val *a) { return emit3(c, op, rt, a, NULL, NULL); }

/* copy v into a fresh temp (so it can be an lvalue / to break aliasing) */
static struct val copy_val(struct cc *c, const struct val *v)
{
    if (is_matrix(v->type)) {
        struct val r = temp(c, v->type);
        int n = vec_size(v->type);
        for (int i = 0; i < n; i++) {
            char ds[64], ss[64];
            struct val col = *v; col.type = vec_of(T_FLOAT, n);
            struct val rc = r; rc.type = col.type;
            dst_str(ds, sizeof ds, &rc, i, n);
            src_str(ss, sizeof ss, &col, i);
            emit_inst(c, "MOV", ds, ss, NULL, NULL);
        }
        return r;
    }
    return emit1(c, "MOV", v->type, v);
}

/* v as a scalar from component k */
static struct val comp(const struct val *v, int k)
{
    struct val r = *v;
    r.type = base_of(v->type);
    r.swz[0] = v->swz[k];
    r.swz[1] = r.swz[2] = r.swz[3] = r.swz[0];
    if (v->is_const) r.c[0] = r.c[1] = r.c[2] = r.c[3] = v->c[k];
    r.lvalue = 0;
    return r;
}

/* matrix column i as a vector */
static struct val column(const struct val *m, int i)
{
    struct val r = *m;
    r.type = vec_of(T_FLOAT, vec_size(m->type));
    r.index = m->index + i;
    r.swz[0] = 0; r.swz[1] = 1; r.swz[2] = 2; r.swz[3] = 3;
    r.is_const = 0;
    return r;
}

/* ---- expressions ---------------------------------------------------------- */

static struct val expr(struct cc *c);
static struct val assignment(struct cc *c);
static struct sym *builtin_symbol(struct cc *c, const char *name);
static int parse_state_struct(struct cc *c, const char *name, struct val *out);
static void statement(struct cc *c);
static void block(struct cc *c);
static struct val call_builtin_or_user(struct cc *c, const char *name);

static void assign_to(struct cc *c, struct val *dst, const struct val *src);

static struct val broadcast(struct val v, int n)
{
    /* a scalar used with an n-vector: replicate the component */
    if (vec_size(v.type) == 1 && !is_matrix(v.type)) {
        for (int i = 1; i < 4; i++) v.swz[i] = v.swz[0];
        v.type = vec_of(v.type, n);
        if (v.is_const) for (int i = 1; i < 4; i++) v.c[i] = v.c[0];
    }
    return v;
}

static struct val mat_mul_vec(struct cc *c, const struct val *m, const struct val *v)
{
    int n = vec_size(m->type);
    struct val vv = *v;
    enum zgl_type vt = vec_of(T_FLOAT, n);
    if (vec_size(v->type) != n) fail(c, "matrix/vector size mismatch");
    struct val r = temp(c, vt);
    char ds[64], cs[64], xs[64], rs[64];
    dst_str(ds, sizeof ds, &r, 0, n);
    src_str(rs, sizeof rs, &r, 0);
    for (int i = 0; i < n; i++) {
        struct val col = column(m, i), x = comp(&vv, i);
        src_str(cs, sizeof cs, &col, 0);
        src_str(xs, sizeof xs, &x, 0);
        if (i == 0) emit_inst(c, "MUL", ds, cs, xs, NULL);
        else emit_inst(c, "MAD", ds, cs, xs, rs);
    }
    return r;
}

static struct val vec_mul_mat(struct cc *c, const struct val *v, const struct val *m)
{
    int n = vec_size(m->type);
    struct val r = temp(c, vec_of(T_FLOAT, n));
    static const char *dp[] = { NULL, NULL, "DP2", "DP3", "DP4" };
    for (int i = 0; i < n; i++) {
        struct val col = column(m, i), rc = comp(&r, i);
        char ds[64], vs[64], cs[64];
        dst_str(ds, sizeof ds, &rc, 0, 1);
        src_str(vs, sizeof vs, v, 0);
        src_str(cs, sizeof cs, &col, 0);
        emit_inst(c, dp[n], ds, vs, cs, NULL);
    }
    return r;
}

static struct val mat_mul_mat(struct cc *c, const struct val *a, const struct val *b)
{
    int n = vec_size(a->type);
    struct val r = temp(c, a->type);
    for (int i = 0; i < n; i++) {
        struct val bc = column(b, i);
        struct val col = mat_mul_vec(c, a, &bc);
        struct val rc = column(&r, i);
        char ds[64], ss[64];
        dst_str(ds, sizeof ds, &rc, 0, n);
        src_str(ss, sizeof ss, &col, 0);
        emit_inst(c, "MOV", ds, ss, NULL, NULL);
    }
    return r;
}

static struct val binary(struct cc *c, const char *op, struct val a, struct val b)
{
    if (is_matrix(a.type) || is_matrix(b.type)) {
        if (strcmp(op, "*") == 0) {
            if (is_matrix(a.type) && is_matrix(b.type)) return mat_mul_mat(c, &a, &b);
            if (is_matrix(a.type) && vec_size(b.type) == 1) {
                struct val r = temp(c, a.type);
                int n = vec_size(a.type);
                for (int i = 0; i < n; i++) {
                    struct val col = column(&a, i), rc = column(&r, i), s = broadcast(b, n);
                    char ds[64], cs[64], ss[64];
                    dst_str(ds, sizeof ds, &rc, 0, n); src_str(cs, sizeof cs, &col, 0); src_str(ss, sizeof ss, &s, 0);
                    emit_inst(c, "MUL", ds, cs, ss, NULL);
                }
                return r;
            }
            if (is_matrix(a.type)) return mat_mul_vec(c, &a, &b);
            return vec_mul_mat(c, &a, &b);
        }
        if (is_matrix(a.type) && is_matrix(b.type) && (strcmp(op, "+") == 0 || strcmp(op, "-") == 0)) {
            struct val r = temp(c, a.type);
            int n = vec_size(a.type);
            for (int i = 0; i < n; i++) {
                struct val ca = column(&a, i), cb = column(&b, i), rc = column(&r, i);
                if (op[0] == '-') cb.neg = !cb.neg;
                char ds[64], as[64], bs[64];
                dst_str(ds, sizeof ds, &rc, 0, n); src_str(as, sizeof as, &ca, 0); src_str(bs, sizeof bs, &cb, 0);
                emit_inst(c, "ADD", ds, as, bs, NULL);
            }
            return r;
        }
        fail(c, "unsupported matrix operation '%s'", op);
    }
    int n = vec_size(a.type) > vec_size(b.type) ? vec_size(a.type) : vec_size(b.type);
    if (vec_size(a.type) != n && vec_size(a.type) != 1) fail(c, "vector size mismatch in '%s'", op);
    if (vec_size(b.type) != n && vec_size(b.type) != 1) fail(c, "vector size mismatch in '%s'", op);
    enum zgl_type rt = vec_of(base_of(a.type) == T_INT && base_of(b.type) == T_INT ? T_INT : T_FLOAT, n);
    a = broadcast(a, n); b = broadcast(b, n);
    /* constant folding for the simple arithmetic keeps unrolled loops tidy */
    if (a.is_const && b.is_const && n == 1 && strchr("+-*/", op[0]) && op[1] == 0) {
        float x = a.c[0], y = b.c[0], r;
        switch (op[0]) { case '+': r = x + y; break; case '-': r = x - y; break; case '*': r = x * y; break; default: r = y != 0 ? x / y : 0; }
        if (rt == T_INT) r = (float)(int)r;
        return imm(c, rt, r, r, r, r);
    }
    if (strcmp(op, "+") == 0) return emit2(c, "ADD", rt, &a, &b);
    if (strcmp(op, "-") == 0) { b.neg = !b.neg; return emit2(c, "ADD", rt, &a, &b); }
    if (strcmp(op, "*") == 0) return emit2(c, "MUL", rt, &a, &b);
    if (strcmp(op, "/") == 0) {
        struct val rb = emit1(c, "RCP", vec_of(T_FLOAT, 1), &b);
        if (n > 1) {
            /* RCP is scalar: one per component */
            struct val r = temp(c, vec_of(T_FLOAT, n));
            for (int i = 0; i < n; i++) {
                struct val bi = comp(&b, i), ri = comp(&r, i);
                char ds[64], bs[64];
                dst_str(ds, sizeof ds, &ri, 0, 1); src_str(bs, sizeof bs, &bi, 0);
                emit_inst(c, "RCP", ds, bs, NULL, NULL);
            }
            rb = r;
        }
        struct val q = emit2(c, "MUL", vec_of(T_FLOAT, n), &a, &rb);
        if (rt == vec_of(T_INT, n)) q = emit1(c, "TRUNC", rt, &q);
        return q;
    }
    if (strcmp(op, "<") == 0) return emit2(c, "SLT", vec_of(T_BOOL, n), &a, &b);
    if (strcmp(op, ">") == 0) return emit2(c, "SGT", vec_of(T_BOOL, n), &a, &b);
    if (strcmp(op, "<=") == 0) return emit2(c, "SLE", vec_of(T_BOOL, n), &a, &b);
    if (strcmp(op, ">=") == 0) return emit2(c, "SGE", vec_of(T_BOOL, n), &a, &b);
    if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0) {
        struct val e = emit2(c, "SEQ", vec_of(T_BOOL, n), &a, &b);
        struct val all = comp(&e, 0);
        for (int i = 1; i < n; i++) { struct val ei = comp(&e, i); all = emit2(c, "MUL", T_BOOL, &all, &ei); }
        if (op[0] == '!') { struct val one = scalar_imm(c, 1.0f); all.neg = 1; all = emit2(c, "ADD", T_BOOL, &one, &all); }
        return all;
    }
    if (strcmp(op, "&&") == 0) return emit2(c, "MUL", T_BOOL, &a, &b);
    if (strcmp(op, "||") == 0) return emit2(c, "MAX", T_BOOL, &a, &b);
    if (strcmp(op, "^^") == 0) { struct val s = emit2(c, "SNE", T_BOOL, &a, &b); return s; }
    fail(c, "unsupported operator '%s'", op);
}

/* Swizzle / field selection on a vector value. */
static struct val swizzle(struct cc *c, struct val v, const char *s)
{
    int n = (int)strlen(s);
    if (n < 1 || n > 4) fail(c, "bad swizzle .%s", s);
    if (is_matrix(v.type)) fail(c, "swizzle on a matrix");
    int size = vec_size(v.type);
    char swz[4] = { 0 };
    float cc[4] = { 0 };
    int in_order = 1;
    for (int i = 0; i < n; i++) {
        int k;
        switch (s[i]) {
        case 'x': case 'r': case 's': k = 0; break;
        case 'y': case 'g': case 't': k = 1; break;
        case 'z': case 'b': case 'p': k = 2; break;
        case 'w': case 'a': case 'q': k = 3; break;
        default: fail(c, "bad swizzle .%s", s);
        }
        if (k >= size) fail(c, "swizzle .%s out of range for %s", s, type_name(v.type));
        swz[i] = v.swz[k];
        cc[i] = v.c[k];
        if (k != i) in_order = 0;
    }
    struct val r = v;
    r.type = vec_of(base_of(v.type), n);
    for (int i = 0; i < 4; i++) { r.swz[i] = swz[i < n ? i : n - 1]; r.c[i] = cc[i < n ? i : n - 1]; }
    r.lvalue = v.lvalue && in_order;        /* out-of-order write masks are handled by assign_to per component */
    if (v.lvalue) { r.lvalue = 1; }
    return r;
}

static struct val index_value(struct cc *c, struct val v, struct val idx, struct sym *s)
{
    if (!idx.is_const) fail(c, "only constant array indices are supported (loops with constant bounds are unrolled)");
    int i = (int)idx.c[0];
    if (s && s->array) {
        if (i < 0 || i >= s->array) fail(c, "array index %d out of range", i);
        struct val r = v;
        r.index = v.index + i * regs_of(s->type);
        r.type = s->type;
        return r;
    }
    if (is_matrix(v.type)) {
        if (i < 0 || i >= vec_size(v.type)) fail(c, "matrix column %d out of range", i);
        struct val r = column(&v, i);
        r.lvalue = v.lvalue;
        return r;
    }
    if (i < 0 || i >= vec_size(v.type)) fail(c, "component %d out of range", i);
    struct val r = comp(&v, i);
    r.lvalue = v.lvalue;
    return r;
}

static struct val primary(struct cc *c)
{
    struct val v;
    memset(&v, 0, sizeof v);
    if (c->tok.kind == TK_FLOAT) { v = scalar_imm(c, (float)c->tok.num); next(c); return v; }
    if (c->tok.kind == TK_INT) { v = imm(c, T_INT, (float)c->tok.num, 0, 0, 0); next(c); return v; }
    if (accept(c, "(")) { v = expr(c); expect(c, ")"); return v; }
    if (accept(c, "true")) return imm(c, T_BOOL, 1, 1, 1, 1);
    if (accept(c, "false")) return imm(c, T_BOOL, 0, 0, 0, 0);
    if (c->tok.kind == TK_IDENT) {
        char name[64];
        strcpy(name, c->tok.text);
        next(c);
        if (is(c, "(")) return call_builtin_or_user(c, name);
        if (parse_state_struct(c, name, &v)) return v;
        struct sym *s = sym_find(c, name);
        if (!s) s = builtin_symbol(c, name);
        if (!s) fail(c, "'%s' undeclared", name);
        v = sym_val(c, s);
        if (s->array) {
            if (!is(c, "[")) fail(c, "array '%s' used without an index", name);
            next(c);
            struct val idx = expr(c);
            expect(c, "]");
            v = index_value(c, v, idx, s);
        }
        return v;
    }
    fail(c, "unexpected '%s' in expression", c->tok.text);
}

static struct val postfix(struct cc *c)
{
    struct val v = primary(c);
    for (;;) {
        if (accept(c, ".")) {
            if (c->tok.kind != TK_IDENT) fail(c, "field name expected");
            char f[64]; strcpy(f, c->tok.text); next(c);
            if (strcmp(f, "length") == 0) { expect(c, "("); expect(c, ")"); v = scalar_imm(c, (float)vec_size(v.type)); continue; }
            v = swizzle(c, v, f);
        } else if (accept(c, "[")) {
            struct val idx = expr(c);
            expect(c, "]");
            v = index_value(c, v, idx, NULL);
        } else if (is(c, "++") || is(c, "--")) {
            struct val one = scalar_imm(c, is(c, "++") ? 1.0f : -1.0f);
            next(c);
            struct val old = copy_val(c, &v);
            struct val nv = binary(c, "+", v, one);
            assign_to(c, &v, &nv);
            v = old;
        } else break;
    }
    return v;
}

static struct val unary(struct cc *c)
{
    if (accept(c, "-")) { struct val v = unary(c); if (v.is_const) { for (int i = 0; i < 4; i++) v.c[i] = -v.c[i]; return imm(c, v.type, v.c[0], v.c[1], v.c[2], v.c[3]); } if (is_matrix(v.type)) { struct val z = imm(c, T_FLOAT, -1, -1, -1, -1); return binary(c, "*", v, z); } v.neg = !v.neg; v.lvalue = 0; return v; }
    if (accept(c, "+")) return unary(c);
    if (accept(c, "!")) { struct val v = unary(c); struct val one = scalar_imm(c, 1.0f); v.neg = !v.neg; return emit2(c, "ADD", T_BOOL, &one, &v); }
    if (is(c, "++") || is(c, "--")) {
        struct val one = scalar_imm(c, is(c, "++") ? 1.0f : -1.0f);
        next(c);
        struct val v = unary(c);
        struct val nv = binary(c, "+", v, one);
        assign_to(c, &v, &nv);
        return v;
    }
    /* a type name followed by ( is a constructor */
    if (c->tok.kind == TK_IDENT && parse_type_name(c->tok.text) != T_STRUCT && peek(c)->kind == TK_PUNCT && strcmp(peek(c)->text, "(") == 0) {
        char name[64]; strcpy(name, c->tok.text); next(c);
        return call_builtin_or_user(c, name);
    }
    return postfix(c);
}

static int prec_of(const char *op)
{
    static const struct { const char *op; int p; } t[] = {
        { "*", 10 }, { "/", 10 }, { "+", 9 }, { "-", 9 },
        { "<", 7 }, { ">", 7 }, { "<=", 7 }, { ">=", 7 }, { "==", 6 }, { "!=", 6 },
        { "&&", 3 }, { "^^", 2 }, { "||", 1 },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (strcmp(t[i].op, op) == 0) return t[i].p;
    return -1;
}

static struct val binary_expr(struct cc *c, int min_prec)
{
    struct val lhs = unary(c);
    for (;;) {
        if (c->tok.kind != TK_PUNCT) break;
        int p = prec_of(c->tok.text);
        if (p < min_prec || p < 0) break;
        char op[4]; strcpy(op, c->tok.text);
        next(c);
        struct val rhs = binary_expr(c, p + 1);
        lhs = binary(c, op, lhs, rhs);
    }
    return lhs;
}

static struct val conditional(struct cc *c)
{
    struct val cond = binary_expr(c, 0);
    if (!accept(c, "?")) return cond;
    struct val a = assignment(c);
    expect(c, ":");
    struct val b = assignment(c);
    int n = vec_size(a.type) > vec_size(b.type) ? vec_size(a.type) : vec_size(b.type);
    a = broadcast(a, n); b = broadcast(b, n);
    struct val cn = broadcast(cond, n); cn.neg = !cn.neg;     /* CMP: src0 < 0 ? src1 : src2 */
    return emit3(c, "CMP", vec_of(base_of(a.type), n), &cn, &a, &b);
}

/* Store src into the lvalue dst (component-wise for permuted swizzles). */
static void assign_to(struct cc *c, struct val *dst, const struct val *src)
{
    if (!dst->lvalue) fail(c, "assignment to a value that is not a variable");
    if (is_matrix(dst->type)) {
        if (dst->type != src->type) fail(c, "matrix assignment type mismatch");
        int n = vec_size(dst->type);
        for (int i = 0; i < n; i++) {
            struct val dc = column(dst, i), sc = column(src, i);
            char ds[64], ss[64];
            dst_str(ds, sizeof ds, &dc, 0, n); src_str(ss, sizeof ss, &sc, 0);
            emit_inst(c, "MOV", ds, ss, NULL, NULL);
        }
        return;
    }
    int n = vec_size(dst->type);
    struct val s = broadcast(*src, n);
    if (vec_size(s.type) != n) fail(c, "assigning %s to %s", type_name(src->type), type_name(dst->type));
    /* if the destination components are in ascending order one MOV does it */
    int ordered = 1;
    for (int i = 1; i < n; i++) if (dst->swz[i] <= dst->swz[i - 1]) ordered = 0;
    if (ordered) {
        char ds[64], ss[64];
        dst_str(ds, sizeof ds, dst, 0, n);
        src_str(ss, sizeof ss, &s, 0);
        emit_inst(c, "MOV", ds, ss, NULL, NULL);
        return;
    }
    struct val tmp = copy_val(c, &s);      /* the source may alias the destination */
    for (int i = 0; i < n; i++) {
        struct val dc = comp(dst, i), sc = comp(&tmp, i);
        char ds[64], ss[64];
        dst_str(ds, sizeof ds, &dc, 0, 1); src_str(ss, sizeof ss, &sc, 0);
        emit_inst(c, "MOV", ds, ss, NULL, NULL);
    }
}

static struct val assignment(struct cc *c)
{
    struct val lhs = conditional(c);
    static const char *ops[] = { "=", "+=", "-=", "*=", "/=", NULL };
    for (int i = 0; ops[i]; i++) {
        if (is(c, ops[i])) {
            next(c);
            struct val rhs = assignment(c);
            if (i > 0) { char op[2] = { ops[i][0], 0 }; rhs = binary(c, op, lhs, rhs); }
            else if (base_of(lhs.type) != base_of(rhs.type) && vec_size(lhs.type) == vec_size(rhs.type)) rhs.type = lhs.type;
            assign_to(c, &lhs, &rhs);
            return lhs;
        }
    }
    return lhs;
}

static struct val expr(struct cc *c)
{
    struct val v = assignment(c);
    while (accept(c, ",")) v = assignment(c);
    return v;
}

/* ---- calls: constructors, built-ins, user functions ----------------------------- */

#define MAX_ARGS 8

static struct val constructor(struct cc *c, enum zgl_type t, struct val *args, int nargs)
{
    if (is_matrix(t)) {
        int n = vec_size(t);
        struct val r = temp(c, t);
        if (nargs == 1 && vec_size(args[0].type) == 1 && !is_matrix(args[0].type)) {
            /* mat(s): s on the diagonal */
            struct val z = scalar_imm(c, 0.0f);
            for (int i = 0; i < n; i++)
                for (int j = 0; j < n; j++) {
                    struct val col = column(&r, i), d = comp(&col, j);
                    char ds[64], ss[64];
                    dst_str(ds, sizeof ds, &d, 0, 1);
                    src_str(ss, sizeof ss, i == j ? &args[0] : &z, 0);
                    emit_inst(c, "MOV", ds, ss, NULL, NULL);
                }
            return r;
        }
        if (nargs == 1 && is_matrix(args[0].type)) {
            /* matN(matM): the top-left, identity elsewhere */
            int m = vec_size(args[0].type);
            struct val z = scalar_imm(c, 0.0f), one = scalar_imm(c, 1.0f);
            for (int i = 0; i < n; i++)
                for (int j = 0; j < n; j++) {
                    struct val col = column(&r, i), d = comp(&col, j);
                    char ds[64], ss[64];
                    dst_str(ds, sizeof ds, &d, 0, 1);
                    if (i < m && j < m) { struct val sc = column(&args[0], i), s = comp(&sc, j); src_str(ss, sizeof ss, &s, 0); }
                    else src_str(ss, sizeof ss, i == j ? &one : &z, 0);
                    emit_inst(c, "MOV", ds, ss, NULL, NULL);
                }
            return r;
        }
        /* from n column vectors, or n*n scalars */
        int k = 0;
        for (int i = 0; i < n; i++) {
            struct val col = column(&r, i);
            if (nargs == n) {
                struct val a = args[i];
                if (vec_size(a.type) != n) fail(c, "matrix constructor column of the wrong size");
                assign_to(c, &col, &a);
            } else {
                for (int j = 0; j < n; j++) {
                    if (k >= nargs) fail(c, "not enough arguments to matrix constructor");
                    struct val d = comp(&col, j), s = comp(&args[k++], 0);
                    char ds[64], ss[64];
                    dst_str(ds, sizeof ds, &d, 0, 1); src_str(ss, sizeof ss, &s, 0);
                    emit_inst(c, "MOV", ds, ss, NULL, NULL);
                }
            }
        }
        return r;
    }
    int n = vec_size(t);
    if (nargs == 1 && args[0].is_const && !is_matrix(args[0].type)) {
        struct val a = args[0];
        float f[4];
        int an = vec_size(a.type);
        for (int i = 0; i < 4; i++) f[i] = a.c[an == 1 ? 0 : (i < an ? i : an - 1)];
        if (base_of(t) == T_INT) for (int i = 0; i < 4; i++) f[i] = (float)(int)f[i];
        if (base_of(t) == T_BOOL) for (int i = 0; i < 4; i++) f[i] = f[i] != 0 ? 1.0f : 0.0f;
        return imm(c, t, f[0], f[1], f[2], f[3]);
    }
    if (nargs == 1 && vec_size(args[0].type) == 1 && !is_matrix(args[0].type)) {
        struct val a = broadcast(args[0], n);
        if (base_of(t) == T_INT && base_of(a.type) == T_FLOAT) { a.type = vec_of(T_FLOAT, n); struct val r = emit1(c, "TRUNC", t, &a); return r; }
        if (base_of(t) == T_BOOL && base_of(a.type) != T_BOOL) { struct val z = scalar_imm(c, 0.0f); a.type = vec_of(T_FLOAT, n); return emit2(c, "SNE", t, &a, &z); }
        a.type = t;
        return a;
    }
    if (nargs == 1 && is_matrix(args[0].type)) {
        struct val col = column(&args[0], 0);
        if (vec_size(col.type) < n) fail(c, "vector constructor from a smaller matrix");
        col.type = t;
        return col;
    }
    /* pack components in order */
    struct val r = temp(c, t);
    int k = 0;
    for (int i = 0; i < nargs && k < n; i++) {
        struct val a = args[i];
        if (is_matrix(a.type)) fail(c, "matrix in vector constructor");
        int an = vec_size(a.type);
        for (int j = 0; j < an && k < n; j++) {
            struct val d = comp(&r, k++), s = comp(&a, j);
            if (base_of(t) == T_INT && base_of(a.type) == T_FLOAT) { s.type = T_FLOAT; struct val ts = emit1(c, "TRUNC", T_INT, &s); s = ts; }
            char ds[64], ss[64];
            dst_str(ds, sizeof ds, &d, 0, 1); src_str(ss, sizeof ss, &s, 0);
            emit_inst(c, "MOV", ds, ss, NULL, NULL);
        }
    }
    if (k < n) fail(c, "not enough components in %s constructor", type_name(t));
    return r;
}

static struct val scalarize(struct cc *c, const char *op, struct val a)
{
    /* per-component scalar instructions (RCP, RSQ, EX2, LG2, SIN, COS, SQRT, POW) */
    int n = vec_size(a.type);
    if (n == 1) return emit1(c, op, vec_of(T_FLOAT, 1), &a);
    struct val r = temp(c, vec_of(T_FLOAT, n));
    for (int i = 0; i < n; i++) {
        struct val ai = comp(&a, i), ri = comp(&r, i);
        char ds[64], as[64];
        dst_str(ds, sizeof ds, &ri, 0, 1); src_str(as, sizeof as, &ai, 0);
        emit_inst(c, op, ds, as, NULL, NULL);
    }
    return r;
}

static struct val scalarize2(struct cc *c, const char *op, struct val a, struct val b)
{
    int n = vec_size(a.type) > vec_size(b.type) ? vec_size(a.type) : vec_size(b.type);
    a = broadcast(a, n); b = broadcast(b, n);
    if (n == 1) return emit2(c, op, T_FLOAT, &a, &b);
    struct val r = temp(c, vec_of(T_FLOAT, n));
    for (int i = 0; i < n; i++) {
        struct val ai = comp(&a, i), bi = comp(&b, i), ri = comp(&r, i);
        char ds[64], as[64], bs[64];
        dst_str(ds, sizeof ds, &ri, 0, 1); src_str(as, sizeof as, &ai, 0); src_str(bs, sizeof bs, &bi, 0);
        emit_inst(c, op, ds, as, bs, NULL);
    }
    return r;
}

static struct val dot(struct cc *c, struct val a, struct val b)
{
    int n = vec_size(a.type);
    if (vec_size(b.type) != n) fail(c, "dot() of different sizes");
    static const char *dp[] = { NULL, "MUL", "DP2", "DP3", "DP4" };
    return emit2(c, dp[n], T_FLOAT, &a, &b);
}

static struct val builtin(struct cc *c, const char *name, struct val *a, int n)
{
    int vn = n ? vec_size(a[0].type) : 0;
    enum zgl_type ft = vec_of(T_FLOAT, vn);
#define ARGS(k) do { if (n != (k)) fail(c, "%s() takes %d argument(s)", name, (k)); } while (0)
    if (!strcmp(name, "dot")) { ARGS(2); return dot(c, a[0], a[1]); }
    if (!strcmp(name, "length")) { ARGS(1); struct val d = dot(c, a[0], a[0]); return emit1(c, "SQRT", T_FLOAT, &d); }
    if (!strcmp(name, "distance")) { ARGS(2); struct val d = binary(c, "-", a[0], a[1]); struct val dd = dot(c, d, d); return emit1(c, "SQRT", T_FLOAT, &dd); }
    if (!strcmp(name, "normalize")) { ARGS(1); struct val d = dot(c, a[0], a[0]); struct val r = emit1(c, "RSQ", T_FLOAT, &d); struct val rb = broadcast(r, vn); return emit2(c, "MUL", ft, &a[0], &rb); }
    if (!strcmp(name, "cross")) {
        ARGS(2);
        struct val ayzx = swizzle(c, a[0], "yzx"), azxy = swizzle(c, a[0], "zxy"), byzx = swizzle(c, a[1], "yzx"), bzxy = swizzle(c, a[1], "zxy");
        struct val t = emit2(c, "MUL", T_VEC3, &azxy, &byzx);
        t.neg = 1;
        return emit3(c, "MAD", T_VEC3, &ayzx, &bzxy, &t);
    }
    if (!strcmp(name, "reflect")) { ARGS(2); struct val d = dot(c, a[1], a[0]); struct val two = scalar_imm(c, -2.0f); struct val k = emit2(c, "MUL", T_FLOAT, &d, &two); struct val kb = broadcast(k, vn); return emit3(c, "MAD", ft, &a[1], &kb, &a[0]); }
    if (!strcmp(name, "min")) { ARGS(2); int m = vec_size(a[0].type); struct val b = broadcast(a[1], m); return emit2(c, "MIN", a[0].type, &a[0], &b); }
    if (!strcmp(name, "max")) { ARGS(2); int m = vec_size(a[0].type); struct val b = broadcast(a[1], m); return emit2(c, "MAX", a[0].type, &a[0], &b); }
    if (!strcmp(name, "clamp")) { ARGS(3); int m = vec_size(a[0].type); struct val lo = broadcast(a[1], m), hi = broadcast(a[2], m); struct val t = emit2(c, "MAX", a[0].type, &a[0], &lo); return emit2(c, "MIN", a[0].type, &t, &hi); }
    if (!strcmp(name, "mix")) { ARGS(3); int m = vec_size(a[0].type); struct val t = broadcast(a[2], m); return emit3(c, "LRP", a[0].type, &t, &a[1], &a[0]); }
    if (!strcmp(name, "step")) { ARGS(2); int m = vec_size(a[1].type); struct val e = broadcast(a[0], m); return emit2(c, "SGE", vec_of(T_FLOAT, m), &a[1], &e); }
    if (!strcmp(name, "smoothstep")) {
        ARGS(3);
        int m = vec_size(a[2].type);
        struct val e0 = broadcast(a[0], m), e1 = broadcast(a[1], m);
        struct val num = binary(c, "-", a[2], e0), den = binary(c, "-", e1, e0);
        struct val t = binary(c, "/", num, den);
        struct val z = scalar_imm(c, 0.0f), one = scalar_imm(c, 1.0f);
        struct val zb = broadcast(z, m), ob = broadcast(one, m);
        t = emit2(c, "MAX", vec_of(T_FLOAT, m), &t, &zb); t = emit2(c, "MIN", vec_of(T_FLOAT, m), &t, &ob);
        struct val three = broadcast(scalar_imm(c, 3.0f), m), mtwo = broadcast(scalar_imm(c, -2.0f), m);
        struct val q = emit3(c, "MAD", vec_of(T_FLOAT, m), &t, &mtwo, &three);      /* 3 - 2t */
        struct val tt = emit2(c, "MUL", vec_of(T_FLOAT, m), &t, &t);
        return emit2(c, "MUL", vec_of(T_FLOAT, m), &tt, &q);
    }
    if (!strcmp(name, "abs")) { ARGS(1); struct val r = temp(c, a[0].type); char ds[64], as[64]; dst_str(ds, sizeof ds, &r, 0, vn); src_str(as, sizeof as, &a[0], 0); char abs[80]; snprintf(abs, sizeof abs, "|%s|", as[0] == '-' ? as + 1 : as); emit_inst(c, "MOV", ds, abs, NULL, NULL); return r; }
    if (!strcmp(name, "sign")) { ARGS(1); struct val z = broadcast(scalar_imm(c, 0.0f), vn); struct val p = emit2(c, "SLT", ft, &z, &a[0]); struct val m = emit2(c, "SLT", ft, &a[0], &z); m.neg = 1; return emit2(c, "ADD", ft, &p, &m); }
    if (!strcmp(name, "floor")) { ARGS(1); return emit1(c, "FLR", ft, &a[0]); }
    if (!strcmp(name, "ceil")) { ARGS(1); return emit1(c, "CEIL", ft, &a[0]); }
    if (!strcmp(name, "fract")) { ARGS(1); return emit1(c, "FRC", ft, &a[0]); }
    if (!strcmp(name, "mod")) { ARGS(2); struct val b = broadcast(a[1], vn); struct val q = binary(c, "/", a[0], b); struct val f = emit1(c, "FLR", ft, &q); f.neg = 1; return emit3(c, "MAD", ft, &b, &f, &a[0]); }
    if (!strcmp(name, "sqrt")) { ARGS(1); return scalarize(c, "SQRT", a[0]); }
    if (!strcmp(name, "inversesqrt")) { ARGS(1); return scalarize(c, "RSQ", a[0]); }
    if (!strcmp(name, "pow")) { ARGS(2); return scalarize2(c, "POW", a[0], a[1]); }
    if (!strcmp(name, "exp2")) { ARGS(1); return scalarize(c, "EX2", a[0]); }
    if (!strcmp(name, "log2")) { ARGS(1); return scalarize(c, "LG2", a[0]); }
    if (!strcmp(name, "exp")) { ARGS(1); struct val k = broadcast(scalar_imm(c, 1.4426950f), vn); struct val t = emit2(c, "MUL", ft, &a[0], &k); return scalarize(c, "EX2", t); }
    if (!strcmp(name, "log")) { ARGS(1); struct val l = scalarize(c, "LG2", a[0]); struct val k = broadcast(scalar_imm(c, 0.6931472f), vn); return emit2(c, "MUL", ft, &l, &k); }
    if (!strcmp(name, "sin")) { ARGS(1); return scalarize(c, "SIN", a[0]); }
    if (!strcmp(name, "cos")) { ARGS(1); return scalarize(c, "COS", a[0]); }
    if (!strcmp(name, "tan")) { ARGS(1); struct val s = scalarize(c, "SIN", a[0]), co = scalarize(c, "COS", a[0]); return binary(c, "/", s, co); }
    if (!strcmp(name, "radians")) { ARGS(1); struct val k = broadcast(scalar_imm(c, 0.017453292f), vn); return emit2(c, "MUL", ft, &a[0], &k); }
    if (!strcmp(name, "degrees")) { ARGS(1); struct val k = broadcast(scalar_imm(c, 57.29578f), vn); return emit2(c, "MUL", ft, &a[0], &k); }
    if (!strcmp(name, "texture2D") || !strcmp(name, "texture2DProj") || !strcmp(name, "texture2DLod") || !strcmp(name, "textureCube") || !strcmp(name, "texture")) {
        if (n < 2) fail(c, "%s() needs a sampler and coordinates", name);
        if (a[0].file != F_SAMP) fail(c, "%s(): first argument is not a sampler", name);
        int cube = a[0].type == T_SAMPLERCUBE;
        struct val r = temp(c, T_VEC4);
        char ds[64], cs[64], samp[32];
        dst_str(ds, sizeof ds, &r, 0, 4);
        struct val co = a[1];
        if (!strcmp(name, "texture2DLod") && n >= 3) {
            /* TXL: coordinates with the lod in .w */
            struct val t4 = temp(c, T_VEC4);
            struct val xy = swizzle(c, t4, "xy"), w = swizzle(c, t4, "w");
            assign_to(c, &xy, &co);
            assign_to(c, &w, &a[2]);
            co = t4;
        } else if (!strcmp(name, "texture2DProj")) {
            if (vec_size(co.type) == 3) { struct val t4 = temp(c, T_VEC4); struct val xy = swizzle(c, t4, "xy"), w = swizzle(c, t4, "w"); struct val cxy = swizzle(c, co, "xy"), cz = swizzle(c, co, "z"); assign_to(c, &xy, &cxy); assign_to(c, &w, &cz); co = t4; }
        }
        src_str(cs, sizeof cs, &co, 0);
        snprintf(samp, sizeof samp, "SAMP[%d]", a[0].index);
        char tgt[96];
        snprintf(tgt, sizeof tgt, "%s, %s", samp, cube ? "CUBE" : "2D");
        emit_inst(c, !strcmp(name, "texture2DProj") ? "TXP" : !strcmp(name, "texture2DLod") ? "TXL" : "TEX", ds, cs, tgt, NULL);
        return r;
    }
    if (!strcmp(name, "dFdx")) { ARGS(1); return emit1(c, "DDX", ft, &a[0]); }
    if (!strcmp(name, "dFdy")) { ARGS(1); return emit1(c, "DDY", ft, &a[0]); }
    if (!strcmp(name, "lessThan")) { ARGS(2); return emit2(c, "SLT", vec_of(T_BOOL, vn), &a[0], &a[1]); }
    if (!strcmp(name, "greaterThan")) { ARGS(2); return emit2(c, "SGT", vec_of(T_BOOL, vn), &a[0], &a[1]); }
    if (!strcmp(name, "lessThanEqual")) { ARGS(2); return emit2(c, "SLE", vec_of(T_BOOL, vn), &a[0], &a[1]); }
    if (!strcmp(name, "greaterThanEqual")) { ARGS(2); return emit2(c, "SGE", vec_of(T_BOOL, vn), &a[0], &a[1]); }
    if (!strcmp(name, "equal")) { ARGS(2); return emit2(c, "SEQ", vec_of(T_BOOL, vn), &a[0], &a[1]); }
    if (!strcmp(name, "notEqual")) { ARGS(2); return emit2(c, "SNE", vec_of(T_BOOL, vn), &a[0], &a[1]); }
    if (!strcmp(name, "any")) { ARGS(1); struct val r = comp(&a[0], 0); for (int i = 1; i < vn; i++) { struct val ci = comp(&a[0], i); r = emit2(c, "MAX", T_BOOL, &r, &ci); } return r; }
    if (!strcmp(name, "all")) { ARGS(1); struct val r = comp(&a[0], 0); for (int i = 1; i < vn; i++) { struct val ci = comp(&a[0], i); r = emit2(c, "MUL", T_BOOL, &r, &ci); } return r; }
    if (!strcmp(name, "not")) { ARGS(1); struct val one = broadcast(scalar_imm(c, 1.0f), vn); struct val v = a[0]; v.neg = !v.neg; return emit2(c, "ADD", vec_of(T_BOOL, vn), &one, &v); }
    if (!strcmp(name, "transpose")) {
        ARGS(1);
        int m = vec_size(a[0].type);
        struct val r = temp(c, a[0].type);
        for (int i = 0; i < m; i++)
            for (int j = 0; j < m; j++) {
                struct val rc = column(&r, i), d = comp(&rc, j), sc = column(&a[0], j), s = comp(&sc, i);
                char ds[64], ss[64];
                dst_str(ds, sizeof ds, &d, 0, 1); src_str(ss, sizeof ss, &s, 0);
                emit_inst(c, "MOV", ds, ss, NULL, NULL);
            }
        return r;
    }
    if (!strcmp(name, "faceforward")) { ARGS(3); struct val d = dot(c, a[2], a[1]); struct val z = scalar_imm(c, 0.0f); struct val neg = a[0]; neg.neg = !neg.neg; struct val cond = emit2(c, "SLT", T_FLOAT, &d, &z); struct val cb = broadcast(cond, vn); cb.neg = 1; return emit3(c, "CMP", ft, &cb, &a[0], &neg); }
    fail(c, "unknown function '%s'", name);
#undef ARGS
}

static enum zgl_type parse_type(struct cc *c)
{
    /* precision and other qualifiers before a type are ignored */
    while (is(c, "highp") || is(c, "mediump") || is(c, "lowp") || is(c, "in") || is(c, "out") || is(c, "inout") || is(c, "const")) next(c);
    if (c->tok.kind != TK_IDENT) fail(c, "type expected before '%s'", c->tok.text);
    enum zgl_type t = parse_type_name(c->tok.text);
    if (t == T_STRUCT) fail(c, "unknown type '%s'", c->tok.text);
    next(c);
    return t;
}

/* Inline a user function: bind the arguments, run the body, return the result. */
static struct val inline_call(struct cc *c, struct func *f, struct val *args, int nargs)
{
    struct lexer saved_lx = c->lx;
    struct token saved_tok = c->tok;
    struct val *saved_ret = c->ret_val;
    int saved_had = c->had_return;
    /* jump to the parameter list */
    c->lx.p = f->body; c->lx.have_ahead = 0; c->lx.expand = NULL;
    next(c);
    expect(c, "(");
    scope_push(c);
    struct { struct val *arg; struct sym *local; int out; } outs[MAX_ARGS];
    int nouts = 0, i = 0;
    while (!is(c, ")")) {
        int is_out = 0;
        while (is(c, "in") || is(c, "out") || is(c, "inout") || is(c, "const") || is(c, "highp") || is(c, "mediump") || is(c, "lowp")) { if (is(c, "out") || is(c, "inout")) is_out = 1; next(c); }
        enum zgl_type t = parse_type(c);
        if (c->tok.kind != TK_IDENT) fail(c, "parameter name expected");
        char pname[48]; strncpy(pname, c->tok.text, 47); pname[47] = 0; next(c);
        if (i >= nargs) fail(c, "too few arguments to %s()", f->name);
        struct sym *s;
        if (is_sampler(t)) {
            s = sym_add(c, pname, t, 0, F_SAMP, args[i].index);
        } else if (args[i].is_const && !is_out && !is_matrix(t)) {
            /* a constant argument stays a constant (array indices in unrolled loops) */
            if (base_of(t) == T_INT && vec_size(t) == 1) { s = sym_add(c, pname, t, 0, F_IMM, 0); s->is_const_int = 1; s->ival = (int)args[i].c[0]; }
            else { struct val im = imm(c, t, args[i].c[0], args[i].c[1], args[i].c[2], args[i].c[3]); s = sym_add(c, pname, t, 0, F_IMM, im.index); }
        } else {
            struct val local = copy_val(c, &args[i]);
            if (base_of(t) != base_of(args[i].type) && vec_size(t) == vec_size(args[i].type)) local.type = t;
            s = sym_add(c, pname, t, 0, F_TEMP, local.index);
            if (is_out) { outs[nouts].arg = &args[i]; outs[nouts].local = s; outs[nouts].out = 1; nouts++; }
        }
        i++;
        if (!accept(c, ",")) break;
    }
    expect(c, ")");
    if (i != nargs) fail(c, "wrong number of arguments to %s()", f->name);
    struct val ret;
    memset(&ret, 0, sizeof ret);
    if (f->ret != T_VOID) ret = temp(c, f->ret);
    c->ret_val = f->ret != T_VOID ? &ret : NULL;
    c->had_return = 0;
    c->inlining++;
    int saved_ret_scope = c->ret_scope;
    c->ret_scope = c->scope + 1;
    block(c);
    c->ret_scope = saved_ret_scope;
    c->inlining--;
    for (int k = 0; k < nouts; k++) {
        struct val lv = sym_val(c, outs[k].local);
        assign_to(c, outs[k].arg, &lv);
    }
    scope_pop(c);
    c->lx = saved_lx; c->tok = saved_tok;
    c->ret_val = saved_ret; c->had_return = saved_had;
    ret.lvalue = 0;
    return ret;
}

static struct val call_builtin_or_user(struct cc *c, const char *name)
{
    struct val args[MAX_ARGS];
    int n = 0;
    expect(c, "(");
    while (!is(c, ")")) {
        if (n >= MAX_ARGS) fail(c, "too many arguments");
        args[n++] = assignment(c);
        if (!accept(c, ",")) break;
    }
    expect(c, ")");
    enum zgl_type t = parse_type_name(name);
    if (t != T_STRUCT && t != T_VOID) return constructor(c, t, args, n);
    for (int i = 0; i < c->nfuncs; i++)
        if (strcmp(c->funcs[i].name, name) == 0) return inline_call(c, &c->funcs[i], args, n);
    return builtin(c, name, args, n);
}

/* ---- declarations ---------------------------------------------------------- */

static int lookup_varying_index(struct cc *c, const char *name, enum zgl_type t, int array)
{
    /* VS: assign in order; FS: the VS's numbering */
    if (c->stage == GL_FRAGMENT_SHADER && c->vs) {
        for (int i = 0; i < c->vs->nvaryings; i++)
            if (strcmp(c->vs->varyings[i].name, name) == 0) return c->vs->varyings[i].slot;
        return -1;
    }
    int slot = 0;
    for (int i = 0; i < c->out->nvaryings; i++) slot += regs_of(c->out->varyings[i].type) * (c->out->varyings[i].array ? c->out->varyings[i].array : 1);
    (void)t; (void)array;
    return slot;
}

static void add_var(struct zgl_var *tab, int *n, const char *name, enum zgl_type t, int array, int slot, int slots, int builtin)
{
    if (*n >= ZGL_MAX_VARS) return;
    struct zgl_var *v = &tab[(*n)++];
    memset(v, 0, sizeof *v);
    strncpy(v->name, name, sizeof v->name - 1);
    v->type = t; v->array = array; v->slot = slot; v->slots = slots; v->builtin = builtin;
}

static void declare_interface(struct cc *c, const char *qual, enum zgl_type t, const char *name, int array)
{
    int count = array ? array : 1;
    if (strcmp(qual, "attribute") == 0) {
        if (c->stage != GL_VERTEX_SHADER) fail(c, "attribute in a fragment shader");
        int idx = c->nin;
        if (is_matrix(t)) fail(c, "matrix attributes are not supported");
        sym_add(c, name, t, array, F_IN, idx);
        for (int i = 0; i < count; i++) snprintf(c->in_decl_txt[c->nin + i], 96, "DCL IN[%d]\n", c->nin + i);
        add_var(c->out->attribs, &c->out->nattribs, name, t, array, idx, count, 0);
        c->nin += count;
        return;
    }
    if (strcmp(qual, "varying") == 0) {
        int regs = regs_of(t) * count;
        if (c->stage == GL_VERTEX_SHADER) {
            int slot = lookup_varying_index(c, name, t, array);
            int idx = c->nout;
            sym_add(c, name, t, array, F_OUT, idx);
            for (int i = 0; i < regs; i++) snprintf(c->out_decl_txt[idx + i], 96, "DCL OUT[%d], GENERIC[%d]\n", idx + i, slot + i);
            add_var(c->out->varyings, &c->out->nvaryings, name, t, array, slot, regs, 0);
            c->nout += regs;
        } else {
            int slot = lookup_varying_index(c, name, t, array);
            int idx = c->nin;
            sym_add(c, name, t, array, F_IN, idx);
            if (slot >= 0)
                for (int i = 0; i < regs; i++) snprintf(c->in_decl_txt[idx + i], 96, "DCL IN[%d], GENERIC[%d], PERSPECTIVE\n", idx + i, slot + i);
            else
                for (int i = 0; i < regs; i++) snprintf(c->in_decl_txt[idx + i], 96, "DCL IN[%d], GENERIC[%d], PERSPECTIVE\n", idx + i, 30 + idx + i);   /* unfed: a spare index */
            add_var(c->out->varyings, &c->out->nvaryings, name, t, array, slot, regs, 0);
            c->nin += regs;
        }
        return;
    }
    if (strcmp(qual, "uniform") == 0) {
        if (is_sampler(t)) {
            int idx = c->nsamp;
            sym_add(c, name, t, array, F_SAMP, idx);
            add_var(c->out->samplers, &c->out->nsamplers, name, t, array, idx, count, 0);
            c->nsamp += count;
            return;
        }
        int regs = regs_of(t) * count;
        int idx = c->nconst;
        sym_add(c, name, t, array, F_CONST, idx);
        add_var(c->out->uniforms, &c->out->nuniforms, name, t, array, idx, regs, 0);
        c->nconst += regs;
        return;
    }
    fail(c, "bad qualifier '%s'", qual);
}

/* the compat built-ins: attributes, state uniforms, outputs; declared on first use */
static struct sym *builtin_symbol(struct cc *c, const char *name)
{
    struct { const char *n; enum zgl_type t; int attr; int state; int array; } tab[] = {
        { "gl_Vertex", T_VEC4, ZGL_ATTR_VERTEX, 0, 0 }, { "gl_Normal", T_VEC3, ZGL_ATTR_NORMAL, 0, 0 },
        { "gl_Color", T_VEC4, ZGL_ATTR_COLOR, 0, 0 }, { "gl_MultiTexCoord0", T_VEC4, ZGL_ATTR_TEXCOORD, 0, 0 },
        { "gl_MultiTexCoord1", T_VEC4, ZGL_ATTR_TEXCOORD + 1, 0, 0 },
        { "gl_ModelViewProjectionMatrix", T_MAT4, -1, ZGL_STATE_MVP, 0 },
        { "gl_ModelViewMatrix", T_MAT4, -1, ZGL_STATE_MV, 0 },
        { "gl_ProjectionMatrix", T_MAT4, -1, ZGL_STATE_PROJ, 0 },
        { "gl_NormalMatrix", T_MAT3, -1, ZGL_STATE_NORMAL, 0 },
        { "gl_ModelViewMatrixInverseTranspose", T_MAT4, -1, ZGL_STATE_MV_INV_T, 0 },
        { "gl_TextureMatrix", T_MAT4, -1, ZGL_STATE_TEXTURE0_MATRIX, 1 },
        { "gl_LightModelAmbient", T_VEC4, -1, ZGL_STATE_LIGHTMODEL_AMBIENT, 0 },
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++) {
        if (strcmp(tab[i].n, name) != 0) continue;
        if (tab[i].attr >= 0) {
            if (c->stage != GL_VERTEX_SHADER) fail(c, "%s in a fragment shader", name);
            /* attributes get IN registers by location order later: here in use order */
            int idx = c->nin++;
            snprintf(c->in_decl_txt[idx], 96, "DCL IN[%d]\n", idx);
            struct sym *s = sym_add(c, name, tab[i].t, 0, F_IN, idx);
            s->scope = 0;
            add_var(c->out->attribs, &c->out->nattribs, name, tab[i].t, 0, idx, 1, tab[i].attr + 1);   /* builtin = location + 1 */
            return s;
        }
        int regs = regs_of(tab[i].t) * (tab[i].array ? tab[i].array : 1);
        struct sym *s = sym_add(c, name, tab[i].t, tab[i].array, F_CONST, c->nconst);
        s->scope = 0;
        add_var(c->out->uniforms, &c->out->nuniforms, name, tab[i].t, tab[i].array, c->nconst, regs, tab[i].state);
        c->nconst += regs;
        return s;
    }
    /* gl_LightSource[i].field and gl_FrontMaterial.field are handled in the parser as structs */
    if (strcmp(name, "gl_FragCoord") == 0 && c->stage == GL_FRAGMENT_SHADER) {
        if (c->fragcoord_in < 0) { c->fragcoord_in = c->nin++; snprintf(c->in_decl_txt[c->fragcoord_in], 96, "DCL IN[%d], POSITION, LINEAR\n", c->fragcoord_in); }
        struct sym *s = sym_add(c, name, T_VEC4, 0, F_IN, c->fragcoord_in); s->scope = 0; return s;
    }
    if (strcmp(name, "gl_FrontFacing") == 0 && c->stage == GL_FRAGMENT_SHADER) {
        if (c->frontfacing_in < 0) { c->frontfacing_in = c->nin++; snprintf(c->in_decl_txt[c->frontfacing_in], 96, "DCL IN[%d], FACE, CONSTANT\n", c->frontfacing_in); }
        struct sym *s = sym_add(c, name, T_BOOL, 0, F_IN, c->frontfacing_in); s->scope = 0; return s;
    }
    if (strcmp(name, "gl_FrontColor") == 0 || strcmp(name, "gl_TexCoord") == 0 || strcmp(name, "gl_FogFragCoord") == 0) {
        /* compat varyings: written by the VS, read by the FS under the same name */
        enum zgl_type t = strcmp(name, "gl_FogFragCoord") == 0 ? T_FLOAT : T_VEC4;
        int array = strcmp(name, "gl_TexCoord") == 0 ? 4 : 0;
        declare_interface(c, "varying", t, name, array);
        return sym_find(c, name);
    }
    if (strcmp(name, "gl_PointSize") == 0 && c->stage == GL_VERTEX_SHADER) {
        if (c->pointsize_out < 0) { c->pointsize_out = c->nout++; snprintf(c->out_decl_txt[c->pointsize_out], 96, "DCL OUT[%d], PSIZE\n", c->pointsize_out); }
        struct sym *s = sym_add(c, name, T_FLOAT, 0, F_OUT, c->pointsize_out); s->scope = 0; return s;
    }
    return NULL;
}

/* gl_LightSource[i].x / gl_FrontMaterial.x / gl_LightModel.ambient as state uniforms */
static int parse_state_struct(struct cc *c, const char *name, struct val *out)
{
    static const struct { const char *f; enum zgl_type t; int off; } light_fields[] = {
        { "ambient", T_VEC4, 0 }, { "diffuse", T_VEC4, 1 }, { "specular", T_VEC4, 2 }, { "position", T_VEC4, 3 },
        { "spotDirection", T_VEC3, 4 }, { "spotExponent", T_FLOAT, 5 }, { "spotCutoff", T_FLOAT, 6 }, { "spotCosCutoff", T_FLOAT, 7 },
        { "constantAttenuation", T_FLOAT, 8 }, { "linearAttenuation", T_FLOAT, 9 }, { "quadraticAttenuation", T_FLOAT, 10 },
    };
    static const struct { const char *f; enum zgl_type t; int off; } mat_fields[] = {
        { "emission", T_VEC4, 0 }, { "ambient", T_VEC4, 1 }, { "diffuse", T_VEC4, 2 }, { "specular", T_VEC4, 3 }, { "shininess", T_FLOAT, 4 },
    };
    if (strcmp(name, "gl_LightSource") == 0) {
        expect(c, "[");
        struct val idx = expr(c);
        expect(c, "]");
        if (!idx.is_const) fail(c, "gl_LightSource index must be constant");
        int li = (int)idx.c[0];
        if (li < 0 || li >= ZGL_MAX_LIGHTS) fail(c, "gl_LightSource index out of range");
        expect(c, ".");
        for (size_t i = 0; i < sizeof light_fields / sizeof light_fields[0]; i++) {
            if (strcmp(c->tok.text, light_fields[i].f) != 0) continue;
            next(c);
            /* each light is 11 vec4 slots (some scalars in .x) */
            char uname[48];
            snprintf(uname, sizeof uname, "gl_LightSource[%d]", li);
            struct sym *s = sym_find(c, uname);
            if (!s) {
                s = sym_add(c, uname, T_VEC4, 11, F_CONST, c->nconst); s->scope = 0;
                add_var(c->out->uniforms, &c->out->nuniforms, uname, T_VEC4, 11, c->nconst, 11, ZGL_STATE_LIGHT0 + li);
                c->nconst += 11;
            }
            struct val v = sym_val(c, s);
            v.index += light_fields[i].off;
            v.type = light_fields[i].t;
            *out = v;
            return 1;
        }
        fail(c, "unknown gl_LightSource field '%s'", c->tok.text);
    }
    if (strcmp(name, "gl_FrontMaterial") == 0 || strcmp(name, "gl_BackMaterial") == 0) {
        expect(c, ".");
        for (size_t i = 0; i < sizeof mat_fields / sizeof mat_fields[0]; i++) {
            if (strcmp(c->tok.text, mat_fields[i].f) != 0) continue;
            next(c);
            struct sym *s = sym_find(c, "gl_FrontMaterial");
            if (!s) {
                s = sym_add(c, "gl_FrontMaterial", T_VEC4, 5, F_CONST, c->nconst); s->scope = 0;
                add_var(c->out->uniforms, &c->out->nuniforms, "gl_FrontMaterial", T_VEC4, 5, c->nconst, 5, ZGL_STATE_MATERIAL_FRONT);
                c->nconst += 5;
            }
            struct val v = sym_val(c, s);
            v.index += mat_fields[i].off;
            v.type = mat_fields[i].t;
            *out = v;
            return 1;
        }
        fail(c, "unknown material field '%s'", c->tok.text);
    }
    if (strcmp(name, "gl_LightModel") == 0) {
        expect(c, "."); expect(c, "ambient");
        struct sym *s = builtin_symbol(c, "gl_LightModelAmbient");
        *out = sym_val(c, s);
        return 1;
    }
    return 0;
}

/* ---- statements -------------------------------------------------------------- */

static void local_declaration(struct cc *c, enum zgl_type t)
{
    for (;;) {
        if (c->tok.kind != TK_IDENT) fail(c, "variable name expected");
        char name[48]; strncpy(name, c->tok.text, 47); name[47] = 0; next(c);
        int array = 0;
        if (accept(c, "[")) { struct val n = expr(c); if (!n.is_const) fail(c, "array size must be constant"); array = (int)n.c[0]; expect(c, "]"); }
        int regs = regs_of(t) * (array ? array : 1);
        struct sym *s = sym_add(c, name, t, array, F_TEMP, c->ntemps);
        c->ntemps += regs;
        if (accept(c, "=")) {
            if (array) fail(c, "array initialisers are not supported");
            struct val init = assignment(c);
            struct val dst = sym_val(c, s);
            if (base_of(t) != base_of(init.type) && vec_size(t) == vec_size(init.type) && !is_matrix(t)) init.type = t;
            if (is_matrix(t) && !is_matrix(init.type)) { struct val a[1] = { init }; init = constructor(c, t, a, 1); }
            assign_to(c, &dst, &init);
        }
        if (!accept(c, ",")) break;
    }
    expect(c, ";");
}

static int starts_declaration(struct cc *c)
{
    if (c->tok.kind != TK_IDENT) return 0;
    if (is(c, "const") || is(c, "highp") || is(c, "mediump") || is(c, "lowp")) return 1;
    return parse_type_name(c->tok.text) != T_STRUCT && peek(c)->kind == TK_IDENT;
}

/* try to unroll `for (int i = a; i < b; i++)`; returns 1 if it did */
static int unroll_for(struct cc *c)
{
    struct lexer save_lx = c->lx; struct token save_tok = c->tok;
    /* init: [int] name = const */
    int have_decl = accept(c, "int") || accept(c, "float");
    if (c->tok.kind != TK_IDENT) goto no;
    char name[48]; strncpy(name, c->tok.text, 47); name[47] = 0; next(c);
    if (!accept(c, "=")) goto no;
    if (c->tok.kind != TK_INT && c->tok.kind != TK_FLOAT) goto no;
    int start = (int)c->tok.num; next(c);
    if (!accept(c, ";")) goto no;
    /* cond: name op const */
    if (!is(c, name)) goto no; next(c);
    char op[4]; if (c->tok.kind != TK_PUNCT) goto no; strcpy(op, c->tok.text); next(c);
    if (c->tok.kind != TK_INT && c->tok.kind != TK_FLOAT) goto no;
    int limit = (int)c->tok.num; next(c);
    if (!accept(c, ";")) goto no;
    /* update: name++ / name-- / ++name / name += const */
    int step = 0;
    if (accept(c, "++")) { if (!is(c, name)) goto no; next(c); step = 1; }
    else if (accept(c, "--")) { if (!is(c, name)) goto no; next(c); step = -1; }
    else {
        if (!is(c, name)) goto no; next(c);
        if (accept(c, "++")) step = 1;
        else if (accept(c, "--")) step = -1;
        else if (accept(c, "+=")) { if (c->tok.kind != TK_INT) goto no; step = (int)c->tok.num; next(c); }
        else if (accept(c, "-=")) { if (c->tok.kind != TK_INT) goto no; step = -(int)c->tok.num; next(c); }
        else goto no;
    }
    if (!accept(c, ")")) goto no;
    if (step == 0) goto no;
    /* iterations */
    int iters = 0;
    for (int i = start; ; i += step) {
        int go = !strcmp(op, "<") ? i < limit : !strcmp(op, "<=") ? i <= limit : !strcmp(op, ">") ? i > limit : !strcmp(op, ">=") ? i >= limit : !strcmp(op, "!=") ? i != limit : 0;
        if (!go || iters > 256) break;
        iters++;
    }
    if (iters > 256) goto no;
    struct lexer body_lx = c->lx; struct token body_tok = c->tok;
    int i = start;
    for (int k = 0; k < iters; k++, i += step) {
        c->lx = body_lx; c->tok = body_tok;
        scope_push(c);
        struct sym *s = sym_add(c, name, T_INT, 0, F_IMM, 0);
        s->is_const_int = 1; s->ival = i;
        statement(c);
        scope_pop(c);
    }
    if (iters == 0) {
        /* skip the body */
        c->lx = body_lx; c->tok = body_tok;
        struct outbuf saved = c->code; int saved_n = c->ninst, saved_t = c->ntemps;
        c->code.s = NULL; c->code.len = c->code.cap = 0;
        scope_push(c);
        struct sym *s = sym_add(c, name, T_INT, 0, F_IMM, 0); s->is_const_int = 1; s->ival = start;
        statement(c);
        scope_pop(c);
        free(c->code.s);
        c->code = saved; c->ninst = saved_n; c->ntemps = saved_t;
    }
    (void)have_decl;
    return 1;
no:
    c->lx = save_lx; c->tok = save_tok;
    return 0;
}

static void condition_if(struct cc *c, struct val cond)
{
    char cs[64];
    struct val cv = cond;
    if (vec_size(cv.type) != 1) fail(c, "condition is not a scalar");
    src_str(cs, sizeof cs, &cv, 0);
    emit_flow(c, "IF", cs);
}

static void statement(struct cc *c)
{
    if (is(c, "{")) { block(c); return; }
    if (accept(c, ";")) return;
    if (accept(c, "if")) {
        expect(c, "(");
        struct val cond = expr(c);
        expect(c, ")");
        if (cond.is_const) {
            /* constant condition (unrolled loops): only compile the taken side */
            int taken = cond.c[0] != 0;
            if (taken) { statement(c); if (accept(c, "else")) { struct outbuf saved = c->code; int sn = c->ninst, st = c->ntemps; c->code.s = NULL; c->code.len = c->code.cap = 0; statement(c); free(c->code.s); c->code = saved; c->ninst = sn; c->ntemps = st; } }
            else { struct outbuf saved = c->code; int sn = c->ninst, st = c->ntemps; c->code.s = NULL; c->code.len = c->code.cap = 0; statement(c); free(c->code.s); c->code = saved; c->ninst = sn; c->ntemps = st; if (accept(c, "else")) statement(c); }
            return;
        }
        condition_if(c, cond);
        statement(c);
        if (accept(c, "else")) { emit_flow(c, "ELSE", NULL); statement(c); }
        emit_flow(c, "ENDIF", NULL);
        return;
    }
    if (accept(c, "for")) {
        expect(c, "(");
        if (unroll_for(c)) return;
        /* a general loop: init; BGNLOOP; if (!cond) BRK; body; update; ENDLOOP */
        scope_push(c);
        if (starts_declaration(c)) { enum zgl_type t = parse_type(c); local_declaration(c, t); }
        else { if (!is(c, ";")) expr(c); expect(c, ";"); }
        struct lexer cond_lx = c->lx; struct token cond_tok = c->tok;
        int has_cond = !is(c, ";");
        if (has_cond) { struct outbuf saved = c->code; int sn = c->ninst, st = c->ntemps; c->code.s = NULL; c->code.len = c->code.cap = 0; expr(c); free(c->code.s); c->code = saved; c->ninst = sn; c->ntemps = st; }
        expect(c, ";");
        struct lexer upd_lx = c->lx; struct token upd_tok = c->tok;
        int has_upd = !is(c, ")");
        if (has_upd) { struct outbuf saved = c->code; int sn = c->ninst, st = c->ntemps; c->code.s = NULL; c->code.len = c->code.cap = 0; expr(c); free(c->code.s); c->code = saved; c->ninst = sn; c->ntemps = st; }
        expect(c, ")");
        emit_flow(c, "BGNLOOP", NULL);
        c->loop_depth++;
        if (has_cond) {
            struct lexer body_lx = c->lx; struct token body_tok = c->tok;
            c->lx = cond_lx; c->tok = cond_tok;
            struct val cond = expr(c);
            struct val ncond = cond; ncond.neg = !ncond.neg;
            struct val one = scalar_imm(c, 1.0f);
            struct val not = emit2(c, "ADD", T_BOOL, &one, &ncond);
            condition_if(c, not); emit_flow(c, "BRK", NULL); emit_flow(c, "ENDIF", NULL);
            c->lx = body_lx; c->tok = body_tok;
        }
        statement(c);
        if (has_upd) {
            struct lexer after_lx = c->lx; struct token after_tok = c->tok;
            c->lx = upd_lx; c->tok = upd_tok;
            expr(c);
            c->lx = after_lx; c->tok = after_tok;
        }
        c->loop_depth--;
        emit_flow(c, "ENDLOOP", NULL);
        scope_pop(c);
        return;
    }
    if (accept(c, "while")) {
        expect(c, "(");
        struct lexer cond_lx = c->lx; struct token cond_tok = c->tok;
        { struct outbuf saved = c->code; int sn = c->ninst, st = c->ntemps; c->code.s = NULL; c->code.len = c->code.cap = 0; expr(c); free(c->code.s); c->code = saved; c->ninst = sn; c->ntemps = st; }
        expect(c, ")");
        struct lexer body_lx = c->lx; struct token body_tok = c->tok;
        emit_flow(c, "BGNLOOP", NULL);
        c->lx = cond_lx; c->tok = cond_tok;
        struct val cond = expr(c);
        struct val ncond = cond; ncond.neg = !ncond.neg;
        struct val one = scalar_imm(c, 1.0f);
        struct val not = emit2(c, "ADD", T_BOOL, &one, &ncond);
        condition_if(c, not); emit_flow(c, "BRK", NULL); emit_flow(c, "ENDIF", NULL);
        c->lx = body_lx; c->tok = body_tok;
        c->loop_depth++;
        statement(c);
        c->loop_depth--;
        emit_flow(c, "ENDLOOP", NULL);
        return;
    }
    if (accept(c, "do")) {
        emit_flow(c, "BGNLOOP", NULL);
        c->loop_depth++;
        statement(c);
        c->loop_depth--;
        expect(c, "while"); expect(c, "(");
        struct val cond = expr(c);
        expect(c, ")"); expect(c, ";");
        struct val ncond = cond; ncond.neg = !ncond.neg;
        struct val one = scalar_imm(c, 1.0f);
        struct val not = emit2(c, "ADD", T_BOOL, &one, &ncond);
        condition_if(c, not); emit_flow(c, "BRK", NULL); emit_flow(c, "ENDIF", NULL);
        emit_flow(c, "ENDLOOP", NULL);
        return;
    }
    if (accept(c, "break")) { if (!c->loop_depth) fail(c, "break outside a loop"); emit_flow(c, "BRK", NULL); expect(c, ";"); return; }
    if (accept(c, "continue")) { if (!c->loop_depth) fail(c, "continue outside a loop"); emit_flow(c, "CONT", NULL); expect(c, ";"); return; }
    if (accept(c, "discard")) { if (c->stage != GL_FRAGMENT_SHADER) fail(c, "discard in a vertex shader"); struct val m = scalar_imm(c, -1.0f); char ms[64]; src_str(ms, sizeof ms, &m, 0); emit_flow(c, "KILL_IF", ms); expect(c, ";"); return; }
    if (accept(c, "return")) {
        if (!is(c, ";")) {
            struct val v = expr(c);
            if (!c->ret_val) fail(c, "return with a value outside a function");
            struct val dst = *c->ret_val;
            assign_to(c, &dst, &v);
        }
        expect(c, ";");
        if (c->inlining && c->scope > c->ret_scope) {
            /* a return inside control flow: nothing after it may run, which we cannot express
             * without a jump; the common shape "if (x) return a; return b;" is handled by
             * treating the rest of the function as the else branch — unsupported otherwise */
            fail(c, "return inside control flow is not supported (write the value and fall through instead)");
        }
        c->had_return = 1;
        return;
    }
    if (starts_declaration(c)) {
        enum zgl_type t = parse_type(c);
        local_declaration(c, t);
        return;
    }
    expr(c);
    expect(c, ";");
}

static void block(struct cc *c)
{
    expect(c, "{");
    scope_push(c);
    while (!is(c, "}")) {
        if (c->tok.kind == TK_EOF) fail(c, "unexpected end of shader");
        if (c->had_return && c->inlining) {
            /* skip the rest of an inlined function after its return */
            int depth = 0;
            while (!(depth == 0 && is(c, "}"))) { if (is(c, "{")) depth++; else if (is(c, "}")) depth--; next(c); if (c->tok.kind == TK_EOF) fail(c, "unexpected end of shader"); }
            break;
        }
        statement(c);
    }
    scope_pop(c);
    expect(c, "}");
}

/* skip a balanced { ... } */
static void skip_block(struct cc *c)
{
    expect(c, "{");
    int depth = 1;
    while (depth) {
        if (c->tok.kind == TK_EOF) fail(c, "unexpected end of shader");
        if (is(c, "{")) depth++;
        else if (is(c, "}")) depth--;
        next(c);
    }
}

static void global_declaration(struct cc *c)
{
    char qual[16] = "";
    int is_const = 0;
    for (;;) {
        if (is(c, "attribute") || is(c, "uniform") || is(c, "varying")) { strcpy(qual, c->tok.text); next(c); }
        else if (is(c, "const")) { is_const = 1; next(c); }
        else if (is(c, "invariant") || is(c, "highp") || is(c, "mediump") || is(c, "lowp") || is(c, "centroid") || is(c, "flat") || is(c, "smooth")) next(c);
        else break;
    }
    if (is(c, "precision")) { while (!is(c, ";")) next(c); next(c); return; }
    if (is(c, "struct")) fail(c, "struct is not supported");
    enum zgl_type t = parse_type(c);
    if (c->tok.kind != TK_IDENT) fail(c, "name expected");
    char name[48]; strncpy(name, c->tok.text, 47); name[47] = 0;
    /* a function? */
    if (peek(c)->kind == TK_PUNCT && strcmp(peek(c)->text, "(") == 0) {
        next(c);
        if (strcmp(name, "main") == 0) {
            expect(c, "("); accept(c, "void"); expect(c, ")");
            c->scope = 0;
            block(c);
            return;
        }
        /* prototype or definition: remember where the parameter list starts */
        const char *at = c->lx.have_ahead ? c->lx.p : c->lx.p;
        struct lexer probe = c->lx; struct token ptok = c->tok;
        /* find the '(' position in the source: the current token is '(' */
        (void)probe; (void)ptok;
        /* skip to ')' then see if a body follows */
        const char *param_start = c->lx.p - 1;      /* points at '(' */
        while (*param_start != '(') param_start--;
        int depth = 0;
        do {
            if (is(c, "(")) depth++;
            else if (is(c, ")")) depth--;
            next(c);
        } while (depth > 0);
        if (accept(c, ";")) return;                 /* prototype */
        if (c->nfuncs >= 64) fail(c, "too many functions");
        struct func *f = &c->funcs[c->nfuncs++];
        strncpy(f->name, name, 47); f->name[47] = 0;
        f->ret = t;
        f->body = param_start;
        f->line = c->tok.line;
        skip_block(c);
        (void)at;
        return;
    }
    /* variables */
    for (;;) {
        if (c->tok.kind != TK_IDENT) fail(c, "name expected");
        strncpy(name, c->tok.text, 47); name[47] = 0; next(c);
        int array = 0;
        if (accept(c, "[")) { struct val n = expr(c); if (!n.is_const) fail(c, "array size must be constant"); array = (int)n.c[0]; expect(c, "]"); }
        if (qual[0]) {
            declare_interface(c, qual, t, name, array);
            if (accept(c, "=")) { while (!is(c, ";") && !is(c, ",")) next(c); }
        } else if (is_const && !array) {
            /* global constant: fold when the initialiser is constant, else a temp */
            expect(c, "=");
            struct val init = assignment(c);
            if (init.is_const && !is_matrix(t)) {
                if (base_of(t) == T_INT) for (int i = 0; i < 4; i++) init.c[i] = (float)(int)init.c[i];
                struct val im = imm(c, t, init.c[0], init.c[1], init.c[2], init.c[3]);
                struct sym *s = sym_add(c, name, t, 0, F_IMM, im.index);
                s->scope = 0;
            } else {
                struct sym *s = sym_add(c, name, t, 0, F_TEMP, c->ntemps); s->scope = 0;
                c->ntemps += regs_of(t);
                struct val dst = sym_val(c, s);
                if (is_matrix(t) && !is_matrix(init.type)) { struct val a[1] = { init }; init = constructor(c, t, a, 1); }
                assign_to(c, &dst, &init);
            }
        } else {
            struct sym *s = sym_add(c, name, t, array, F_TEMP, c->ntemps); s->scope = 0;
            c->ntemps += regs_of(t) * (array ? array : 1);
            if (accept(c, "=")) {
                struct val init = assignment(c);
                struct val dst = sym_val(c, s);
                if (is_matrix(t) && !is_matrix(init.type)) { struct val a[1] = { init }; init = constructor(c, t, a, 1); }
                assign_to(c, &dst, &init);
            }
        }
        if (!accept(c, ",")) break;
    }
    expect(c, ";");
}

/* ---- driver -------------------------------------------------------------------- */

/* The real entry: compile `source` of `type`. `vs` is the vertex shader's
 * interface when compiling a fragment shader (varying numbering). */
int zgl_compile_linked(const char *source, GLenum type, const struct zgl_compiled *vs, struct zgl_compiled *out, char *log, size_t logsize)
{
    struct cc *c = calloc(1, sizeof *c);
    if (!c) return -1;
    memset(out, 0, sizeof *out);
    c->stage = type; c->out = out; c->vs = vs; c->log = log; c->logsize = logsize;
    c->frontfacing_in = -1; c->fragcoord_in = -1; c->pointsize_out = -1;
    log[0] = 0;
    lex_init(&c->lx, source);
    if (setjmp(c->err)) {
        free(c->code.s); free(c->decl.s); free(c);
        return -1;
    }
    /* outputs the stage always has */
    if (type == GL_VERTEX_SHADER) {
        snprintf(c->out_decl_txt[0], 96, "DCL OUT[0], POSITION\n");
        c->nout = 1;
        struct sym *s = sym_add(c, "gl_Position", T_VEC4, 0, F_OUT, 0); s->scope = 0;
    } else {
        snprintf(c->out_decl_txt[0], 96, "DCL OUT[0], COLOR\n");
        c->nout = 1;
        struct sym *s = sym_add(c, "gl_FragColor", T_VEC4, 0, F_OUT, 0); s->scope = 0;
        s = sym_add(c, "gl_FragData", T_VEC4, 1, F_OUT, 0); s->scope = 0;
    }
    next(c);
    while (c->tok.kind != TK_EOF)
        global_declaration(c);
    emit_flow(c, "END", NULL);

    /* assemble: header, declarations, immediates, code */
    struct outbuf o = { 0 };
    ob_put(&o, type == GL_VERTEX_SHADER ? "VERT\n" : "FRAG\nPROPERTY FS_COLOR0_WRITES_ALL_CBUFS 1\n");
    for (int i = 0; i < c->nin; i++) ob_put(&o, c->in_decl_txt[i]);
    for (int i = 0; i < c->nout; i++) ob_put(&o, c->out_decl_txt[i]);
    if (c->nconst) obf(&o, "DCL CONST[0..%d]\n", c->nconst - 1);
    for (int i = 0; i < c->nsamp; i++) obf(&o, "DCL SAMP[%d]\nDCL SVIEW[%d], 2D, FLOAT\n", i, i);
    if (c->ntemps) obf(&o, "DCL TEMP[0..%d]\n", c->ntemps - 1);
    for (int i = 0; i < c->nimm; i++) obf(&o, "IMM FLT32 {%.9g, %.9g, %.9g, %.9g}\n", c->imm[i][0], c->imm[i][1], c->imm[i][2], c->imm[i][3]);
    ob_put(&o, c->code.s ? c->code.s : "");
    out->tgsi = o.s;
    out->nconsts = c->nconst;
    free(c->code.s); free(c->decl.s); free(c);
    return 0;
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
