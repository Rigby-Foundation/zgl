/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* The code generator's model, shared by glsl_gen.c (declarations,
 * expressions, statements) and glsl_func.c (built-in functions).
 *
 * Values live in TGSI's vec4 registers: a scalar or vector in one, a matrix
 * in one per column, arrays and structs in consecutive runs of those. All
 * registers hold raw 32-bit words: floats, ints and uints as themselves, a
 * bool as ~0 (true) or 0 (false), which is what the integer compare opcodes
 * produce and UIF tests. */
#pragma once
#include "glsl_int.h"

enum file { F_NONE, F_TEMP, F_IN, F_OUT, F_CONST, F_IMM, F_SV, F_SAMP };

struct val {
    struct gtype t;
    enum file file;
    int index;                  /* the first register */
    int arr;                    /* the register array it lies in (TEMP/IN/OUT ARRAY(id)), 0 if none */
    int ind;                    /* 1: the register is index + TEMP[ind_reg].x (ADDR);
                                 * 2: an input/output array element chosen by TEMP[ind_reg].x,
                                 *    reached by branches (the host has no indirect I/O) */
    int ind_reg;
    int ind_count, ind_stride;  /* ind 2: elements and registers per element */
    unsigned char swz[4];       /* components, for a scalar or vector */
    int neg, abs;               /* source modifiers (float operands only) */
    int lv;                     /* assignable */
    int is_const;               /* the value is known: cv[], column-major */
    uint32_t cv[16];
    int samp;                   /* F_SAMP: the sampler index */
};

struct sym {
    const char *name;
    struct gtype t;
    struct val v;               /* its storage (or constant value) */
    int scope, frame;           /* frame 0: global; else the inlined call it belongs to */
    int quals;
};

struct ufunc {                  /* a user function (all overloads share the name) */
    struct fdecl *d;
    struct gtype ret;
    struct gtype *pt; int np;   /* parameter types */
    int inlining;               /* recursion guard */
};

/* the levels of break-only control flow: every TGSI loop is a do { } while(true) */
enum lvk { LV_LOOP, LV_BODY, LV_SWITCH, LV_FUNC };
struct lvl {
    enum lvk k;
    int brk_flag;               /* LV_LOOP with a body wrapper: set by `break` (a TEMP), else -1 */
    int cont_flag;              /* LV_SWITCH: set by a `continue` passing through, else -1 */
    int ret_flag;               /* LV_FUNC: set by a `return` from inside a loop, else -1 */
    int has_body;               /* LV_LOOP: the body is wrapped (it has a `continue`) */
    struct val *ret;            /* LV_FUNC: where the return value goes (NULL for void) */
};

struct gen {
    struct glc *c;
    /* the instruction stream */
    char *code; size_t len, cap;
    int ninst;
    /* registers */
    int ntemps;
    struct { int first, n; } arrays[512]; int narrays;   /* TEMP ranges declared as ARRAY(i + 1) */
    struct { char k; uint32_t v[4]; } imm[1024]; int nimm;
    char in_decl[96][96]; int nin;
    char out_decl[96][96]; int nout;
    char sv_decl[8][64]; int nsv;
    int dual_src;               /* an `index = 1` output (COLOR[1]) exists */
    int nconst, nsamp;
    enum zgl_type samp_type[64];
    int uses_addr;
    int writes_fragcolor, fragdata_out, fragdepth_out;
    int next_color;             /* the next free COLOR location for an `out` */
    unsigned color_used;
    /* names */
    struct sym *syms; int nsyms, capsyms;
    int scope, frame, nframes;
    struct ufunc *fns; int nfns, capfns;
    struct gstruct **structs; int nstructs, capstructs;
    /* control flow */
    struct lvl lv[64]; int nlv;
    int in_func;                /* > 0 while inlining */
};

/* glsl_gen.c */
struct val v_temp(struct gen *g, struct gtype t);
struct val v_imm(struct gen *g, struct gtype t, const uint32_t *comps);
struct val v_fimm(struct gen *g, float f);
struct val v_iimm(struct gen *g, int i);
struct val v_uimm(struct gen *g, uint32_t u);
struct val v_comp(struct val v, int k);             /* component k as a scalar */
struct val v_col(struct gen *g, struct val m, int k); /* matrix column k */
struct val v_bcast(struct val v, int n);            /* a scalar replicated to n components */
struct val v_swizzle(struct val v, const int *idx, int n);
struct val v_direct(struct gen *g, struct val v);   /* indirect -> a temp copy */
struct val v_convert(struct gen *g, struct val v, enum gk to);
struct val v_copy(struct gen *g, struct val v);     /* into a fresh temp */
void v_store(struct gen *g, struct val dst, struct val src);
struct val op1(struct gen *g, const char *op, struct gtype rt, struct val a);
struct val op2(struct gen *g, const char *op, struct gtype rt, struct val a, struct val b);
struct val op3(struct gen *g, const char *op, struct gtype rt, struct val a, struct val b, struct val c);
struct val scalar_op1(struct gen *g, const char *op, struct val a);           /* per component (RCP, SIN, ...) */
struct val scalar_op2(struct gen *g, const char *op, struct val a, struct val b);
struct val v_binary(struct gen *g, const char *op, struct val a, struct val b);
struct val v_not(struct gen *g, struct val b);
struct val v_select(struct gen *g, struct val cond, struct val a, struct val b);   /* cond ? a : b, per component */
void emit(struct gen *g, const char *op, const char *dst, const char *s0, const char *s1, const char *s2, const char *s3);
void emitf(struct gen *g, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void src_str(struct gen *g, char *buf, size_t n, struct val v, int reg);
void dst_str(struct gen *g, char *buf, size_t n, struct val v, int reg, int ncomp);
float cv_f(const struct val *v, int k);
int32_t cv_i(const struct val *v, int k);
struct gtype elem_type(struct gtype t);             /* an array's element type */

/* glsl_func.c: a call to a built-in function (or texture lookup); returns 0
 * with *out set, or -1 when no built-in has that name */
int call_builtin(struct gen *g, const char *name, struct val *args, int nargs, struct val *out);
int is_builtin_func(const char *name);
