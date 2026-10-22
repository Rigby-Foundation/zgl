/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* The GLSL compiler's insides, shared by its passes:
 *
 *   glsl_pp.c      preprocessor and lexer: source -> tokens
 *   glsl_parse.c   parser: tokens -> syntax tree
 *   glsl_gen.c     types, names, overloads and TGSI generation from the tree
 *   glsl_func.c    the built-in functions
 *   glsl_types.c   the interface types the GL layer sees (zgl.h)
 *
 * Everything a compile allocates comes from one arena, freed at the end; an
 * error longjmps out with the message in the log. */
#pragma once
#include "zgl.h"
#include <setjmp.h>
#include <stdint.h>
#include <stddef.h>

struct glc;

/* ---- tokens ------------------------------------------------------------------ */

enum tk { TK_EOF, TK_IDENT, TK_INT, TK_UINT, TK_FLOAT, TK_PUNCT };

struct tok {
    enum tk kind;
    const char *s;              /* the text, NUL-terminated (arena) */
    double f;                   /* TK_FLOAT */
    uint32_t u;                 /* TK_INT, TK_UINT */
    int line;
};

/* glsl_pp.c: preprocess and tokenize `src` into c->toks; sets c->version */
void pp_run(struct glc *c, const char *src);

/* ---- types ------------------------------------------------------------------- */

enum gk { GK_VOID, GK_FLOAT, GK_INT, GK_UINT, GK_BOOL, GK_SAMPLER, GK_STRUCT };

struct gstruct;

struct gtype {
    enum gk k;
    unsigned char vec;          /* components (1-4); for a matrix its rows */
    unsigned char cols;         /* 0, or the columns of a float matrix */
    int array;                  /* 0: not an array; > 0 elements; -1: size still unknown */
    enum zgl_type sampler;      /* GK_SAMPLER: which one */
    struct gstruct *st;         /* GK_STRUCT */
};

struct gfield { const char *name; struct gtype t; };
struct gstruct { const char *name; struct gfield *f; int n; };

/* ---- the syntax tree --------------------------------------------------------------- */

enum nk {
    /* expressions */
    N_INTLIT, N_UINTLIT, N_FLOATLIT, N_BOOLLIT, N_IDENT,
    N_BINARY, N_UNARY, N_ASSIGN, N_COND, N_CALL, N_FIELD, N_INDEX,
    N_PREINC, N_PREDEC, N_POSTINC, N_POSTDEC, N_COMMA,
    /* statements */
    N_BLOCK, N_DECL, N_EXPR, N_IF, N_FOR, N_WHILE, N_DO, N_SWITCH, N_CASE, N_DEFAULT,
    N_BREAK, N_CONTINUE, N_RETURN, N_DISCARD, N_EMPTY,
};

/* qualifiers of a declaration */
enum {
    Q_CONST = 1, Q_UNIFORM = 2, Q_IN = 4, Q_OUT = 8, Q_ATTRIBUTE = 16, Q_VARYING = 32,
    Q_FLAT = 64, Q_SMOOTH = 128, Q_NOPERSPECTIVE = 256, Q_CENTROID = 512, Q_INVARIANT = 1024,
    Q_FACE = 1 << 20,           /* glsl_gen.c: gl_FrontFacing, a FACE input read as a bool */
};

struct tspec {                  /* a type as written: a name, maybe a struct body, maybe [n] */
    const char *name;           /* "vec4", a struct name; NULL for an anonymous struct */
    struct node **fields;       /* struct { ... }: N_DECL members */
    int nfields;
    struct node *array;         /* [expr] after the type; NULL; for [] an N_EMPTY */
    int has_array;
};

struct node {
    enum nk k;
    int line;
    const char *op;             /* N_BINARY, N_UNARY, N_ASSIGN: the operator */
    const char *name;           /* N_IDENT, N_FIELD, N_CALL (callee), N_DECL (variable) */
    struct node *a, *b, *c, *d; /* operands / cond, then, else / init, cond, step, body */
    struct node **l; int n;     /* N_CALL arguments, N_BLOCK statements, N_SWITCH body */
    struct tspec *ts;           /* N_CALL to a constructor, N_DECL */
    int quals;                  /* N_DECL */
    int location, binding;      /* N_DECL layout(location=, binding=); -1 if absent */
    int index;                  /* N_DECL layout(index=) (dual-source blending), 0 if absent */
    int has_array;              /* N_DECL: name[...] */
    struct node *array;         /* N_DECL: the size expression (N_EMPTY for []) */
    double f; uint32_t u;       /* literals */
};

/* a function definition or prototype */
struct fdecl {
    const char *name;
    struct tspec *ret;
    struct node **params;       /* N_DECL with quals Q_IN / Q_OUT (inout = both), Q_CONST */
    int nparams;
    struct node *body;          /* N_BLOCK, NULL for a prototype */
    int line;
};

/* a top-level declaration */
struct gdecl {
    enum { G_VAR, G_FUNC, G_STRUCT, G_BLOCK, G_PRECISION, G_DEFAULT_LAYOUT } k;
    struct node *var;           /* G_VAR: N_DECL (a list: var->l are the declarators) */
    struct fdecl *fn;
    struct tspec *ts;           /* G_STRUCT, G_BLOCK (the block's members) */
    const char *block_name, *instance; int quals;
    int binding, row_major;     /* G_BLOCK: layout(binding = n) (-1 if none), a row_major qualifier anywhere in it */
};

/* glsl_parse.c: c->toks -> c->decls */
void parse_unit(struct glc *c);

/* ---- the compile ------------------------------------------------------------------- */

struct arena_blk { struct arena_blk *next; size_t used, cap; char data[]; };

struct gen;                     /* glsl_gen.c's state */

struct glc {
    GLenum stage;
    int version;                /* 110, 120, 130, 140, 150, 330, 100 (ES), 300 (ES) */
    int es;
    struct zgl_compiled *out;
    const struct zgl_compiled *vs;
    const char (*frag_loc)[48];  /* glBindFragDataLocation: location -> output name ("" for none), or NULL */
    const char *frag_index1;    /* glBindFragDataLocationIndexed(0, 1): the second-source output's name, or NULL */
    char *log; size_t logsize;
    jmp_buf err;
    struct arena_blk *arena;
    /* tokens */
    struct tok *toks; int ntoks, captoks;
    int pos;                    /* parser position */
    int line;                   /* for errors */
    /* the parsed unit */
    struct gdecl *decls; int ndecls, capdecls;
    struct gen *g;
};

void *ar_alloc(struct glc *c, size_t n);            /* zeroed */
char *ar_strdup(struct glc *c, const char *s);
char *ar_strndup(struct glc *c, const char *s, size_t n);
void __attribute__((noreturn, format(printf, 2, 3))) glc_error(struct glc *c, const char *fmt, ...);

/* glsl_gen.c */
void gen_unit(struct glc *c);

/* type helpers (glsl_gen.c) */
struct gtype ty_scalar(enum gk k);
struct gtype ty_vec(enum gk k, int n);
struct gtype ty_mat(int cols, int rows);
int ty_eq(struct gtype a, struct gtype b);
int ty_regs(struct gtype t);                        /* vec4 registers a value takes */
int ty_elem_regs(struct gtype t);                   /* the same for one array element */
const char *ty_name(struct gtype t, char *buf, size_t n);
int ty_is_scalar(struct gtype t);
int ty_is_vector(struct gtype t);                   /* scalar or vector, not an array */
int ty_is_matrix(struct gtype t);
int ty_is_numeric(struct gtype t);
