/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* The GLSL parser: the token array to a syntax tree (glsl_int.h), by
 * recursive descent. It knows the grammar only; types, names and meaning
 * are glsl_gen.c's. The one thing it has to track is which identifiers name
 * structs, since `S x;` is a declaration and `f x;` is not. */
#include "glsl_int.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- tokens ----------------------------------------------------------------- */

static struct tok *cur(struct glc *c) { return &c->toks[c->pos]; }
static struct tok *peekn(struct glc *c, int n) { int i = c->pos + n; return &c->toks[i < c->ntoks ? i : c->ntoks - 1]; }
static void adv(struct glc *c) { if (c->pos < c->ntoks - 1) c->pos++; c->line = cur(c)->line; }
static int is(struct glc *c, const char *s) { struct tok *t = cur(c); return (t->kind == TK_PUNCT || t->kind == TK_IDENT) && !strcmp(t->s, s); }
static int is_at(struct glc *c, int n, const char *s) { struct tok *t = peekn(c, n); return (t->kind == TK_PUNCT || t->kind == TK_IDENT) && !strcmp(t->s, s); }
static int accept(struct glc *c, const char *s) { if (is(c, s)) { adv(c); return 1; } return 0; }
static void expect(struct glc *c, const char *s)
{
    c->line = cur(c)->line;
    if (!accept(c, s)) glc_error(c, "'%s' expected before '%s'", s, cur(c)->kind == TK_EOF ? "end of shader" : cur(c)->s);
}
static const char *ident(struct glc *c, const char *what)
{
    c->line = cur(c)->line;
    if (cur(c)->kind != TK_IDENT) glc_error(c, "%s expected before '%s'", what, cur(c)->kind == TK_EOF ? "end of shader" : cur(c)->s);
    const char *s = cur(c)->s;
    adv(c);
    return s;
}

static struct node *mk(struct glc *c, enum nk k)
{
    struct node *n = ar_alloc(c, sizeof *n);
    n->k = k;
    n->line = cur(c)->line;
    n->location = n->binding = -1;
    return n;
}

struct nlist { struct node **v; int n, cap; };
static void nl_push(struct glc *c, struct nlist *l, struct node *x)
{
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 8;
        struct node **v = ar_alloc(c, (size_t)cap * sizeof *v);
        if (l->n) memcpy(v, l->v, (size_t)l->n * sizeof *v);
        l->v = v; l->cap = cap;
    }
    l->v[l->n++] = x;
}

/* ---- type names ------------------------------------------------------------------ */

static const char *const builtin_types[] = {
    "void", "float", "int", "uint", "bool", "double",
    "vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4", "uvec2", "uvec3", "uvec4", "bvec2", "bvec3", "bvec4",
    "mat2", "mat3", "mat4", "mat2x2", "mat2x3", "mat2x4", "mat3x2", "mat3x3", "mat3x4", "mat4x2", "mat4x3", "mat4x4",
    "sampler1D", "sampler2D", "sampler3D", "samplerCube", "sampler2DRect", "sampler1DArray", "sampler2DArray",
    "samplerBuffer", "sampler2DMS", "sampler2DMSArray", "samplerCubeArray",
    "sampler1DShadow", "sampler2DShadow", "samplerCubeShadow", "sampler2DRectShadow", "sampler1DArrayShadow",
    "sampler2DArrayShadow", "samplerCubeArrayShadow", "samplerExternalOES",
    "isampler1D", "isampler2D", "isampler3D", "isamplerCube", "isampler2DRect", "isampler1DArray", "isampler2DArray",
    "isamplerBuffer", "isampler2DMS", "isampler2DMSArray", "isamplerCubeArray",
    "usampler1D", "usampler2D", "usampler3D", "usamplerCube", "usampler2DRect", "usampler1DArray", "usampler2DArray",
    "usamplerBuffer", "usampler2DMS", "usampler2DMSArray", "usamplerCubeArray",
    NULL
};

/* struct names in scope: a stack with block marks */
static const char *structs[256];
static int nstructs;

static int is_type_name(struct glc *c, const char *s)
{
    (void)c;
    for (int i = 0; builtin_types[i]; i++) if (!strcmp(builtin_types[i], s)) return 1;
    for (int i = nstructs - 1; i >= 0; i--) if (structs[i] && !strcmp(structs[i], s)) return 1;
    return 0;
}

static int is_qualifier(const char *s)
{
    static const char *const q[] = {
        "const", "uniform", "in", "out", "inout", "attribute", "varying", "centroid", "flat", "smooth",
        "noperspective", "invariant", "highp", "mediump", "lowp", "precise", "layout", "sample", "patch", NULL
    };
    for (int i = 0; q[i]; i++) if (!strcmp(q[i], s)) return 1;
    return 0;
}

/* ---- expressions ------------------------------------------------------------------ */

static struct node *expr(struct glc *c);
static struct node *assignment(struct glc *c);
static struct tspec *type_spec(struct glc *c);
static struct node *declaration(struct glc *c, int quals, int location, int binding, struct tspec *ts);
static struct node *statement(struct glc *c);

static struct node *call_args(struct glc *c, struct node *call)
{
    struct nlist args = { 0 };
    expect(c, "(");
    if (accept(c, "void")) { expect(c, ")"); return call; }
    if (!is(c, ")"))
        for (;;) {
            nl_push(c, &args, assignment(c));
            if (!accept(c, ",")) break;
        }
    expect(c, ")");
    call->l = args.v; call->n = args.n;
    return call;
}

static struct node *primary(struct glc *c)
{
    struct tok *t = cur(c);
    struct node *n;
    c->line = t->line;
    switch (t->kind) {
    case TK_INT: n = mk(c, N_INTLIT); n->u = t->u; adv(c); return n;
    case TK_UINT: n = mk(c, N_UINTLIT); n->u = t->u; adv(c); return n;
    case TK_FLOAT: n = mk(c, N_FLOATLIT); n->f = t->f; adv(c); return n;
    case TK_PUNCT:
        if (accept(c, "(")) { n = expr(c); expect(c, ")"); return n; }
        glc_error(c, "unexpected '%s'", t->s);
    case TK_IDENT:
        if (!strcmp(t->s, "true") || !strcmp(t->s, "false")) { n = mk(c, N_BOOLLIT); n->u = t->s[0] == 't'; adv(c); return n; }
        if (is_type_name(c, t->s)) {
            /* a constructor: type(...) or type[n](...) */
            n = mk(c, N_CALL);
            n->ts = type_spec(c);
            n->name = n->ts->name;
            return call_args(c, n);
        }
        if (is_at(c, 1, "(")) { n = mk(c, N_CALL); n->name = t->s; adv(c); return call_args(c, n); }
        n = mk(c, N_IDENT); n->name = t->s; adv(c);
        return n;
    case TK_EOF:
        glc_error(c, "unexpected end of shader");
    }
    glc_error(c, "unexpected '%s'", t->s);
}

static struct node *postfix(struct glc *c)
{
    struct node *e = primary(c);
    for (;;) {
        if (accept(c, "[")) {
            struct node *n = mk(c, N_INDEX);
            n->a = e; n->b = expr(c);
            expect(c, "]");
            e = n;
        } else if (accept(c, ".")) {
            const char *f = ident(c, "a field name");
            if (is(c, "(")) {               /* a method: x.length() */
                struct node *n = mk(c, N_CALL);
                n->name = f; n->a = e;
                e = call_args(c, n);
            } else {
                struct node *n = mk(c, N_FIELD);
                n->a = e; n->name = f;
                e = n;
            }
        } else if (is(c, "++") || is(c, "--")) {
            struct node *n = mk(c, is(c, "++") ? N_POSTINC : N_POSTDEC);
            adv(c);
            n->a = e;
            e = n;
        } else break;
    }
    return e;
}

static struct node *unary(struct glc *c)
{
    if (is(c, "++") || is(c, "--")) {
        struct node *n = mk(c, is(c, "++") ? N_PREINC : N_PREDEC);
        adv(c);
        n->a = unary(c);
        return n;
    }
    if (is(c, "+") || is(c, "-") || is(c, "!") || is(c, "~")) {
        struct node *n = mk(c, N_UNARY);
        n->op = cur(c)->s;
        adv(c);
        n->a = unary(c);
        return n;
    }
    return postfix(c);
}

static int binprec(struct glc *c)
{
    struct tok *t = cur(c);
    if (t->kind != TK_PUNCT) return -1;
    static const struct { const char *op; int p; } tab[] = {
        { "*", 12 }, { "/", 12 }, { "%", 12 }, { "+", 11 }, { "-", 11 }, { "<<", 10 }, { ">>", 10 },
        { "<", 9 }, { ">", 9 }, { "<=", 9 }, { ">=", 9 }, { "==", 8 }, { "!=", 8 },
        { "&", 7 }, { "^", 6 }, { "|", 5 }, { "&&", 4 }, { "^^", 3 }, { "||", 2 },
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++) if (!strcmp(tab[i].op, t->s)) return tab[i].p;
    return -1;
}

static struct node *binary(struct glc *c, int min)
{
    struct node *a = unary(c);
    for (;;) {
        int p = binprec(c);
        if (p < 0 || p < min) return a;
        struct node *n = mk(c, N_BINARY);
        n->op = cur(c)->s;
        adv(c);
        n->a = a;
        n->b = binary(c, p + 1);
        a = n;
    }
}

static struct node *conditional(struct glc *c)
{
    struct node *cnd = binary(c, 0);
    if (!accept(c, "?")) return cnd;
    struct node *n = mk(c, N_COND);
    n->a = cnd;
    n->b = expr(c);
    expect(c, ":");
    n->c = assignment(c);
    return n;
}

static struct node *assignment(struct glc *c)
{
    struct node *lhs = conditional(c);
    static const char *const ops[] = { "=", "+=", "-=", "*=", "/=", "%=", "<<=", ">>=", "&=", "^=", "|=", NULL };
    for (int i = 0; ops[i]; i++)
        if (is(c, ops[i])) {
            struct node *n = mk(c, N_ASSIGN);
            n->op = ops[i];
            adv(c);
            n->a = lhs;
            n->b = assignment(c);
            return n;
        }
    return lhs;
}

static struct node *expr(struct glc *c)
{
    struct node *e = assignment(c);
    while (is(c, ",")) {
        struct node *n = mk(c, N_COMMA);
        adv(c);
        n->a = e;
        n->b = assignment(c);
        e = n;
    }
    return e;
}

/* ---- types and declarations ------------------------------------------------------- */

static struct node *array_suffix(struct glc *c)
{
    /* after '[': the size, or [] (an N_EMPTY) */
    struct node *n;
    if (is(c, "]")) n = mk(c, N_EMPTY);
    else n = conditional(c);
    expect(c, "]");
    if (is(c, "[")) glc_error(c, "arrays of arrays are not supported");
    return n;
}

static struct node *struct_member_decl(struct glc *c)
{
    while (is(c, "highp") || is(c, "mediump") || is(c, "lowp") || is(c, "precise")) adv(c);
    struct tspec *ts = type_spec(c);
    struct node *d = mk(c, N_DECL);
    d->ts = ts;
    struct nlist vars = { 0 };
    for (;;) {
        struct node *v = mk(c, N_DECL);
        v->name = ident(c, "a member name");
        if (accept(c, "[")) { v->has_array = 1; v->array = array_suffix(c); }
        nl_push(c, &vars, v);
        if (!accept(c, ",")) break;
    }
    expect(c, ";");
    d->l = vars.v; d->n = vars.n;
    return d;
}

static struct tspec *type_spec(struct glc *c)
{
    struct tspec *ts = ar_alloc(c, sizeof *ts);
    c->line = cur(c)->line;
    if (accept(c, "struct")) {
        if (cur(c)->kind == TK_IDENT) ts->name = ident(c, "a struct name");
        expect(c, "{");
        struct nlist m = { 0 };
        while (!is(c, "}")) {
            if (cur(c)->kind == TK_EOF) glc_error(c, "unterminated struct");
            nl_push(c, &m, struct_member_decl(c));
        }
        expect(c, "}");
        ts->fields = m.v; ts->nfields = m.n;
        if (!ts->name) {
            char b[32];
            static int anon;
            snprintf(b, sizeof b, "__anon%d", anon++);
            ts->name = ar_strdup(c, b);
        }
        if (nstructs < 256) structs[nstructs++] = ts->name;
    } else {
        if (cur(c)->kind != TK_IDENT || !is_type_name(c, cur(c)->s)) glc_error(c, "a type expected before '%s'", cur(c)->kind == TK_EOF ? "end of shader" : cur(c)->s);
        ts->name = cur(c)->s;
        adv(c);
    }
    if (accept(c, "[")) { ts->has_array = 1; ts->array = array_suffix(c); }
    return ts;
}

/* qualifiers before a declaration: the Q_* bits, and layout(location/binding) */
static int layout_index;        /* layout(index = n) of the declaration being parsed */

static int qualifiers(struct glc *c, int *location, int *binding, int *block_layout)
{
    int q = 0;
    *location = *binding = -1;
    layout_index = 0;
    for (;;) {
        struct tok *t = cur(c);
        if (t->kind != TK_IDENT || !is_qualifier(t->s)) break;
        const char *s = t->s;
        adv(c);
        if (!strcmp(s, "layout")) {
            expect(c, "(");
            for (;;) {
                const char *k = ident(c, "a layout qualifier");
                int v = -1;
                if (accept(c, "=")) {
                    if (cur(c)->kind != TK_INT && cur(c)->kind != TK_UINT) glc_error(c, "layout(%s =) needs an integer", k);
                    v = (int)cur(c)->u;
                    adv(c);
                }
                if (!strcmp(k, "location")) *location = v;
                else if (!strcmp(k, "binding")) *binding = v;
                else if (!strcmp(k, "index")) layout_index = v;
                else if (block_layout && (!strcmp(k, "std140") || !strcmp(k, "shared") || !strcmp(k, "packed") || !strcmp(k, "std430"))) *block_layout = 1;
                /* row_major, column_major, origin_upper_left, pixel_center_integer, index: accepted */
                if (!accept(c, ",")) break;
            }
            expect(c, ")");
        }
        else if (!strcmp(s, "const")) q |= Q_CONST;
        else if (!strcmp(s, "uniform")) q |= Q_UNIFORM;
        else if (!strcmp(s, "in")) q |= Q_IN;
        else if (!strcmp(s, "out")) q |= Q_OUT;
        else if (!strcmp(s, "inout")) q |= Q_IN | Q_OUT;
        else if (!strcmp(s, "attribute")) q |= Q_ATTRIBUTE;
        else if (!strcmp(s, "varying")) q |= Q_VARYING;
        else if (!strcmp(s, "centroid")) q |= Q_CENTROID;
        else if (!strcmp(s, "flat")) q |= Q_FLAT;
        else if (!strcmp(s, "smooth")) q |= Q_SMOOTH;
        else if (!strcmp(s, "noperspective")) q |= Q_NOPERSPECTIVE;
        else if (!strcmp(s, "invariant")) q |= Q_INVARIANT;
        /* precision, precise, sample, patch: no effect here */
    }
    return q;
}

/* declarators after the type: name[n] = init, ... ; */
static struct node *declaration(struct glc *c, int quals, int location, int binding, struct tspec *ts)
{
    struct node *d = mk(c, N_DECL);
    d->ts = ts; d->quals = quals; d->location = location; d->binding = binding; d->index = layout_index;
    struct nlist vars = { 0 };
    if (!is(c, ";"))
        for (;;) {
            struct node *v = mk(c, N_DECL);
            v->name = ident(c, "a variable name");
            if (accept(c, "[")) { v->has_array = 1; v->array = array_suffix(c); }
            if (accept(c, "=")) {
                if (is(c, "{")) glc_error(c, "initializer lists are not supported (use a constructor)");
                v->a = assignment(c);
            }
            nl_push(c, &vars, v);
            if (!accept(c, ",")) break;
        }
    expect(c, ";");
    d->l = vars.v; d->n = vars.n;
    return d;
}

/* ---- statements ---------------------------------------------------------------------- */

/* does a declaration start here? (a type or qualifier, then a name) */
static int starts_decl(struct glc *c)
{
    struct tok *t = cur(c);
    if (t->kind != TK_IDENT) return 0;
    if (!strcmp(t->s, "struct")) return 1;
    if (is_qualifier(t->s)) return 1;
    if (!is_type_name(c, t->s)) return 0;
    struct tok *n = peekn(c, 1);
    if (n->kind == TK_IDENT) return 1;
    if (n->kind == TK_PUNCT && !strcmp(n->s, "[")) {
        /* type[n] name (a declaration) or type[n](...) (a constructor) */
        int i = 2, depth = 1;
        for (; depth && peekn(c, i)->kind != TK_EOF; i++) {
            if (is_at(c, i, "[")) depth++;
            else if (is_at(c, i, "]")) depth--;
        }
        return peekn(c, i)->kind == TK_IDENT;
    }
    return 0;
}

static struct node *block(struct glc *c)
{
    struct node *b = mk(c, N_BLOCK);
    struct nlist s = { 0 };
    int mark = nstructs;
    expect(c, "{");
    while (!is(c, "}")) {
        if (cur(c)->kind == TK_EOF) glc_error(c, "'}' expected before the end of the shader");
        nl_push(c, &s, statement(c));
    }
    expect(c, "}");
    nstructs = mark;
    b->l = s.v; b->n = s.n;
    return b;
}

static struct node *decl_statement(struct glc *c)
{
    int loc, bind;
    int q = qualifiers(c, &loc, &bind, NULL);
    if (is(c, "precision")) glc_error(c, "unexpected 'precision'");
    struct tspec *ts = type_spec(c);
    if (is(c, ";") && ts->nfields) { adv(c); return mk(c, N_EMPTY); }      /* struct S { ... }; */
    return declaration(c, q, loc, bind, ts);
}

static struct node *statement(struct glc *c)
{
    struct node *n;
    c->line = cur(c)->line;
    if (is(c, "{")) return block(c);
    if (accept(c, ";")) return mk(c, N_EMPTY);
    if (is(c, "precision")) { while (!accept(c, ";")) adv(c); return mk(c, N_EMPTY); }
    if (accept(c, "if")) {
        n = mk(c, N_IF);
        expect(c, "(");
        n->a = expr(c);
        expect(c, ")");
        n->b = statement(c);
        if (accept(c, "else")) n->c = statement(c);
        return n;
    }
    if (accept(c, "for")) {
        n = mk(c, N_FOR);
        int mark = nstructs;
        expect(c, "(");
        if (accept(c, ";")) n->a = NULL;
        else if (starts_decl(c)) n->a = decl_statement(c);
        else { n->a = mk(c, N_EXPR); n->a->a = expr(c); expect(c, ";"); }
        if (!is(c, ";")) n->b = expr(c);
        expect(c, ";");
        if (!is(c, ")")) n->c = expr(c);
        expect(c, ")");
        n->d = statement(c);
        nstructs = mark;
        return n;
    }
    if (accept(c, "while")) {
        n = mk(c, N_WHILE);
        expect(c, "(");
        n->a = expr(c);
        expect(c, ")");
        n->b = statement(c);
        return n;
    }
    if (accept(c, "do")) {
        n = mk(c, N_DO);
        n->b = statement(c);
        expect(c, "while");
        expect(c, "(");
        n->a = expr(c);
        expect(c, ")");
        expect(c, ";");
        return n;
    }
    if (accept(c, "switch")) {
        n = mk(c, N_SWITCH);
        expect(c, "(");
        n->a = expr(c);
        expect(c, ")");
        n->b = block(c);
        return n;
    }
    if (accept(c, "case")) { n = mk(c, N_CASE); n->a = conditional(c); expect(c, ":"); return n; }
    if (accept(c, "default")) { n = mk(c, N_DEFAULT); expect(c, ":"); return n; }
    if (accept(c, "break")) { expect(c, ";"); return mk(c, N_BREAK); }
    if (accept(c, "continue")) { expect(c, ";"); return mk(c, N_CONTINUE); }
    if (accept(c, "discard")) { expect(c, ";"); return mk(c, N_DISCARD); }
    if (accept(c, "return")) {
        n = mk(c, N_RETURN);
        if (!is(c, ";")) n->a = expr(c);
        expect(c, ";");
        return n;
    }
    if (starts_decl(c)) return decl_statement(c);
    n = mk(c, N_EXPR);
    n->a = expr(c);
    expect(c, ";");
    return n;
}

/* ---- the top level ----------------------------------------------------------------- */

static struct gdecl *add_decl(struct glc *c)
{
    if (c->ndecls == c->capdecls) {
        int cap = c->capdecls ? c->capdecls * 2 : 32;
        struct gdecl *n = ar_alloc(c, (size_t)cap * sizeof *n);
        if (c->ndecls) memcpy(n, c->decls, (size_t)c->ndecls * sizeof *n);
        c->decls = n; c->capdecls = cap;
    }
    struct gdecl *d = &c->decls[c->ndecls++];
    memset(d, 0, sizeof *d);
    return d;
}

static void params(struct glc *c, struct fdecl *f)
{
    struct nlist l = { 0 };
    expect(c, "(");
    if (is(c, "void") && is_at(c, 1, ")")) adv(c);
    if (!is(c, ")"))
        for (;;) {
            int loc, bind;
            int q = qualifiers(c, &loc, &bind, NULL);
            struct node *p = mk(c, N_DECL);
            p->quals = q ? q : Q_IN;
            p->ts = type_spec(c);
            if (cur(c)->kind == TK_IDENT) p->name = ident(c, "a parameter name");
            if (accept(c, "[")) { p->has_array = 1; p->array = array_suffix(c); }
            nl_push(c, &l, p);
            if (!accept(c, ",")) break;
        }
    expect(c, ")");
    f->params = l.v; f->nparams = l.n;
}

static void external(struct glc *c)
{
    if (accept(c, ";")) return;
    c->line = cur(c)->line;
    if (is(c, "precision")) {
        while (!accept(c, ";")) { if (cur(c)->kind == TK_EOF) glc_error(c, "';' expected"); adv(c); }
        add_decl(c)->k = G_PRECISION;
        return;
    }
    int loc, bind, block_layout = 0;
    int q = qualifiers(c, &loc, &bind, &block_layout);
    if (is(c, ";")) {                   /* layout(std140) uniform; */
        adv(c);
        add_decl(c)->k = G_DEFAULT_LAYOUT;
        return;
    }
    /* an interface block: uniform Name { ... } [instance]; */
    if (cur(c)->kind == TK_IDENT && !is_type_name(c, cur(c)->s) && strcmp(cur(c)->s, "struct") && is_at(c, 1, "{")) {
        if (!(q & (Q_UNIFORM | Q_IN | Q_OUT))) glc_error(c, "an interface block needs uniform, in or out");
        struct gdecl *d = add_decl(c);
        d->k = G_BLOCK;
        d->quals = q;
        d->block_name = ident(c, "a block name");
        expect(c, "{");
        struct tspec *ts = ar_alloc(c, sizeof *ts);
        ts->name = d->block_name;
        struct nlist m = { 0 };
        while (!is(c, "}")) {
            if (cur(c)->kind == TK_EOF) glc_error(c, "unterminated block");
            int l2, b2;
            qualifiers(c, &l2, &b2, &block_layout);
            nl_push(c, &m, struct_member_decl(c));
        }
        expect(c, "}");
        ts->fields = m.v; ts->nfields = m.n;
        d->ts = ts;
        if (cur(c)->kind == TK_IDENT) {
            d->instance = ident(c, "an instance name");
            if (accept(c, "[")) glc_error(c, "arrays of interface blocks are not supported");
        }
        expect(c, ";");
        return;
    }
    struct tspec *ts = type_spec(c);
    if (ts->nfields && is(c, ";")) {    /* struct S { ... }; */
        adv(c);
        struct gdecl *d = add_decl(c);
        d->k = G_STRUCT;
        d->ts = ts;
        return;
    }
    if (ts->nfields) { struct gdecl *d = add_decl(c); d->k = G_STRUCT; d->ts = ts; }
    /* a function? */
    if (cur(c)->kind == TK_IDENT && is_at(c, 1, "(")) {
        struct fdecl *f = ar_alloc(c, sizeof *f);
        f->line = cur(c)->line;
        f->name = ident(c, "a function name");
        f->ret = ts;
        params(c, f);
        if (!accept(c, ";")) {
            int mark = nstructs;
            f->body = block(c);
            nstructs = mark;
        }
        struct gdecl *d = add_decl(c);
        d->k = G_FUNC;
        d->fn = f;
        return;
    }
    struct gdecl *d = add_decl(c);
    d->k = G_VAR;
    d->var = declaration(c, q, loc, bind, ts);
}

void parse_unit(struct glc *c)
{
    nstructs = 0;
    c->pos = 0;
    c->line = cur(c)->line;
    while (cur(c)->kind != TK_EOF) external(c);
}
