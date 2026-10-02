/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* The TGSI text parser. It takes what zgl's GLSL compiler writes and no
 * more: declarations, immediates, instructions with swizzles, negation,
 * |abs| and _SAT, structured control flow. */
#include "tgsi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static const char *op_names[OP_COUNT] = {
    [OP_NOP] = "NOP", [OP_MOV] = "MOV", [OP_ADD] = "ADD", [OP_MUL] = "MUL", [OP_MAD] = "MAD", [OP_DP2] = "DP2",
    [OP_DP3] = "DP3", [OP_DP4] = "DP4", [OP_MIN] = "MIN", [OP_MAX] = "MAX", [OP_SLT] = "SLT", [OP_SGE] = "SGE",
    [OP_SEQ] = "SEQ", [OP_SNE] = "SNE", [OP_SGT] = "SGT", [OP_SLE] = "SLE", [OP_CMP] = "CMP", [OP_LRP] = "LRP",
    [OP_FRC] = "FRC", [OP_FLR] = "FLR", [OP_TRUNC] = "TRUNC", [OP_CEIL] = "CEIL", [OP_RCP] = "RCP", [OP_RSQ] = "RSQ",
    [OP_SQRT] = "SQRT", [OP_EX2] = "EX2", [OP_LG2] = "LG2", [OP_POW] = "POW", [OP_SIN] = "SIN", [OP_COS] = "COS",
    [OP_DDX] = "DDX", [OP_DDY] = "DDY", [OP_TEX] = "TEX", [OP_TXP] = "TXP", [OP_TXL] = "TXL", [OP_KILL_IF] = "KILL_IF",
    [OP_IF] = "IF", [OP_ELSE] = "ELSE", [OP_ENDIF] = "ENDIF", [OP_BGNLOOP] = "BGNLOOP", [OP_ENDLOOP] = "ENDLOOP",
    [OP_BRK] = "BRK", [OP_CONT] = "CONT", [OP_END] = "END",
};
static const uint8_t op_nsrc[OP_COUNT] = {
    [OP_MOV] = 1, [OP_ADD] = 2, [OP_MUL] = 2, [OP_MAD] = 3, [OP_DP2] = 2, [OP_DP3] = 2, [OP_DP4] = 2, [OP_MIN] = 2,
    [OP_MAX] = 2, [OP_SLT] = 2, [OP_SGE] = 2, [OP_SEQ] = 2, [OP_SNE] = 2, [OP_SGT] = 2, [OP_SLE] = 2, [OP_CMP] = 3,
    [OP_LRP] = 3, [OP_FRC] = 1, [OP_FLR] = 1, [OP_TRUNC] = 1, [OP_CEIL] = 1, [OP_RCP] = 1, [OP_RSQ] = 1, [OP_SQRT] = 1,
    [OP_EX2] = 1, [OP_LG2] = 1, [OP_POW] = 2, [OP_SIN] = 1, [OP_COS] = 1, [OP_DDX] = 1, [OP_DDY] = 1, [OP_TEX] = 1,
    [OP_TXP] = 1, [OP_TXL] = 1, [OP_KILL_IF] = 1, [OP_IF] = 1,
};

const char *tgsi_op_name(int op) { return op >= 0 && op < OP_COUNT && op_names[op] ? op_names[op] : "?"; }

struct parser { const char *p; char *err; int errsize; int line; };

static int fail(struct parser *ps, const char *msg)
{
    const char *e = ps->p;
    int n = 0;
    while (e[n] && e[n] != '\n' && n < 40) n++;
    snprintf(ps->err, (size_t)ps->errsize, "tgsi line %d: %s at '%.*s'", ps->line, msg, n, e);
    return -1;
}
static void skip_ws(struct parser *ps) { while (*ps->p == ' ' || *ps->p == '\t') ps->p++; }
static int accept(struct parser *ps, const char *s)
{
    skip_ws(ps);
    size_t n = strlen(s);
    if (strncmp(ps->p, s, n) == 0) { ps->p += n; return 1; }
    return 0;
}
static int word(struct parser *ps, char *out, int size)
{
    skip_ws(ps);
    int n = 0;
    while ((isalnum((unsigned char)*ps->p) || *ps->p == '_') && n < size - 1) out[n++] = *ps->p++;
    out[n] = 0;
    return n;
}
static int number(struct parser *ps, long *v)
{
    skip_ws(ps);
    char *end;
    *v = strtol(ps->p, &end, 10);
    if (end == ps->p) return 0;
    ps->p = end;
    return 1;
}
static void next_line(struct parser *ps)
{
    while (*ps->p && *ps->p != '\n') ps->p++;
    if (*ps->p == '\n') { ps->p++; ps->line++; }
}

static int file_of(const char *w)
{
    if (!strcmp(w, "IN")) return TF_IN;
    if (!strcmp(w, "OUT")) return TF_OUT;
    if (!strcmp(w, "TEMP")) return TF_TEMP;
    if (!strcmp(w, "CONST")) return TF_CONST;
    if (!strcmp(w, "IMM")) return TF_IMM;
    if (!strcmp(w, "SAMP")) return TF_SAMP;
    return -1;
}

/* FILE[n] (the file already read) */
static int reg_index(struct parser *ps, long *idx)
{
    if (!accept(ps, "[") || !number(ps, idx) || !accept(ps, "]")) return fail(ps, "expected [index]");
    return 0;
}

static int swizzle(struct parser *ps, uint8_t swz[4], int *count)
{
    *count = 0;
    if (!accept(ps, ".")) return 0;
    while (*count < 4) {
        char c = *ps->p;
        int v = c == 'x' ? 0 : c == 'y' ? 1 : c == 'z' ? 2 : c == 'w' ? 3 : -1;
        if (v < 0) break;
        swz[(*count)++] = (uint8_t)v;
        ps->p++;
    }
    return 0;
}

static int parse_src(struct parser *ps, struct tgsi_src *s)
{
    memset(s, 0, sizeof *s);
    for (int i = 0; i < 4; i++) s->swz[i] = (uint8_t)i;
    skip_ws(ps);
    if (accept(ps, "-")) s->neg = 1;
    if (accept(ps, "|")) s->abs = 1;
    char w[16];
    if (!word(ps, w, sizeof w)) return fail(ps, "expected a register");
    int f = file_of(w);
    if (f < 0) return fail(ps, "unknown register file");
    long idx;
    if (reg_index(ps, &idx)) return -1;
    s->file = (uint8_t)f;
    s->index = (uint16_t)idx;
    int n;
    swizzle(ps, s->swz, &n);
    if (n == 1) s->swz[1] = s->swz[2] = s->swz[3] = s->swz[0];
    if (s->abs && !accept(ps, "|")) return fail(ps, "expected |");
    return 0;
}

static int parse_dst(struct parser *ps, struct tgsi_dst *d)
{
    char w[16];
    if (!word(ps, w, sizeof w)) return fail(ps, "expected a destination");
    int f = file_of(w);
    if (f != TF_TEMP && f != TF_OUT) return fail(ps, "bad destination file");
    long idx;
    if (reg_index(ps, &idx)) return -1;
    d->file = (uint8_t)f;
    d->index = (uint16_t)idx;
    d->mask = 0xF;
    if (accept(ps, ".")) {
        d->mask = 0;
        for (;;) {
            char c = *ps->p;
            int v = c == 'x' ? 0 : c == 'y' ? 1 : c == 'z' ? 2 : c == 'w' ? 3 : -1;
            if (v < 0) break;
            d->mask |= (uint8_t)(1 << v);
            ps->p++;
        }
    }
    return 0;
}

static int parse_decl(struct parser *ps, struct tgsi_shader *s)
{
    char w[16];
    if (!word(ps, w, sizeof w)) return fail(ps, "expected a declaration");
    int f = file_of(w);
    long lo, hi;
    if (!strcmp(w, "SVIEW")) { next_line(ps); return 0; }
    if (f < 0) return fail(ps, "unknown declaration");
    if (!accept(ps, "[") || !number(ps, &lo)) return fail(ps, "expected [n]");
    hi = lo;
    if (accept(ps, "..") && !number(ps, &hi)) return fail(ps, "expected [a..b]");
    if (!accept(ps, "]")) return fail(ps, "expected ]");
    if (f == TF_TEMP) { if (hi + 1 > s->ntemps) s->ntemps = (int)hi + 1; }
    else if (f == TF_CONST) { if (hi + 1 > s->nconsts) s->nconsts = (int)hi + 1; }
    else if (f == TF_SAMP) { if (hi + 1 > s->nsamp) s->nsamp = (int)hi + 1; }
    else if (f == TF_IN || f == TF_OUT) {
        if (hi >= TGSI_MAX_IO) return fail(ps, "too many inputs/outputs");
        struct tgsi_io io = { SEM_NONE, 0, 0 };
        if (accept(ps, ",")) {
            char sem[16];
            word(ps, sem, sizeof sem);
            long si = 0;
            if (accept(ps, "[") && (!number(ps, &si) || !accept(ps, "]"))) return fail(ps, "bad semantic index");
            io.sem = !strcmp(sem, "POSITION") ? SEM_POSITION : !strcmp(sem, "COLOR") ? SEM_COLOR : !strcmp(sem, "BCOLOR") ? SEM_BCOLOR :
                     !strcmp(sem, "GENERIC") ? SEM_GENERIC : !strcmp(sem, "PSIZE") ? SEM_PSIZE : !strcmp(sem, "FACE") ? SEM_FACE :
                     !strcmp(sem, "FOG") ? SEM_FOG : SEM_NONE;
            io.sindex = (uint16_t)si;
            if (accept(ps, ",")) {
                char in[16];
                word(ps, in, sizeof in);
                io.interp = !strcmp(in, "LINEAR") ? 1 : !strcmp(in, "CONSTANT") ? 2 : 0;
            }
        }
        for (long i = lo; i <= hi; i++) {
            if (f == TF_IN) { s->in[i] = io; if (i + 1 > s->nin) s->nin = (int)i + 1; }
            else { s->out[i] = io; if (i + 1 > s->nout) s->nout = (int)i + 1; }
            if (io.sem == SEM_GENERIC) io.sindex++;
        }
    }
    next_line(ps);
    return 0;
}

static int parse_imm(struct parser *ps, struct tgsi_shader *s)
{
    if (s->nimm >= TGSI_MAX_IMM) return fail(ps, "too many immediates");
    if (!accept(ps, "FLT32") || !accept(ps, "{")) return fail(ps, "only FLT32 immediates");
    float *v = s->imm[s->nimm];
    for (int i = 0; i < 4; i++) {
        skip_ws(ps);
        char *end;
        v[i] = strtof(ps->p, &end);
        if (end == ps->p) return fail(ps, "bad immediate");
        ps->p = end;
        if (i < 3 && !accept(ps, ",")) return fail(ps, "expected ,");
    }
    if (!accept(ps, "}")) return fail(ps, "expected }");
    s->nimm++;
    next_line(ps);
    return 0;
}

static int lookup_op(const char *w, int *sat)
{
    char name[24];
    snprintf(name, sizeof name, "%s", w);
    *sat = 0;
    size_t n = strlen(name);
    if (n > 4 && !strcmp(name + n - 4, "_SAT")) { name[n - 4] = 0; *sat = 1; }
    for (int i = 0; i < OP_COUNT; i++) if (op_names[i] && !strcmp(op_names[i], name)) return i;
    return -1;
}

int tgsi_parse(const char *text, struct tgsi_shader *s, char *err, int errsize)
{
    memset(s, 0, sizeof *s);
    struct parser ps = { text, err, errsize, 1 };
    int cap = 64;
    s->inst = malloc(sizeof *s->inst * (size_t)cap);
    int stack[32], sp = 0;
    while (*ps.p) {
        skip_ws(&ps);
        if (*ps.p == '\n') { ps.p++; ps.line++; continue; }
        if (!*ps.p) break;
        char w[24];
        /* "  12: OP ..." */
        if (isdigit((unsigned char)*ps.p)) { long ln; number(&ps, &ln); if (!accept(&ps, ":")) return fail(&ps, "expected :"); }
        if (!word(&ps, w, sizeof w)) { next_line(&ps); continue; }
        if (!strcmp(w, "VERT")) { s->frag = 0; next_line(&ps); continue; }
        if (!strcmp(w, "FRAG")) { s->frag = 1; next_line(&ps); continue; }
        if (!strcmp(w, "PROPERTY")) { next_line(&ps); continue; }
        if (!strcmp(w, "DCL")) { if (parse_decl(&ps, s)) return -1; continue; }
        if (!strcmp(w, "IMM")) { if (parse_imm(&ps, s)) return -1; continue; }
        int sat, op = lookup_op(w, &sat);
        if (op < 0) return fail(&ps, "unknown opcode");
        if (s->ninst == cap) { cap *= 2; s->inst = realloc(s->inst, sizeof *s->inst * (size_t)cap); }
        struct tgsi_inst *in = &s->inst[s->ninst];
        memset(in, 0, sizeof *in);
        in->op = (uint8_t)op;
        in->sat = (uint8_t)sat;
        in->jump = -1;
        int has_dst = op_nsrc[op] > 0 && op != OP_KILL_IF && op != OP_IF;
        if (has_dst) {
            if (parse_dst(&ps, &in->dst)) return -1;
            if (!accept(&ps, ",")) return fail(&ps, "expected , after the destination");
        }
        for (int i = 0; i < op_nsrc[op]; i++) {
            if (i && !accept(&ps, ",")) return fail(&ps, "expected ,");
            if (parse_src(&ps, &in->src[i])) return -1;
        }
        in->nsrc = op_nsrc[op];
        if (op == OP_TEX || op == OP_TXP || op == OP_TXL) {
            struct tgsi_src samp;
            if (!accept(&ps, ",") || parse_src(&ps, &samp) || samp.file != TF_SAMP) return fail(&ps, "expected SAMP[n]");
            in->sampler = samp.index;
            char tgt[16] = "";
            if (accept(&ps, ",")) word(&ps, tgt, sizeof tgt);
            in->tex_target = !strcmp(tgt, "CUBE") ? 1 : 0;
        }
        /* structured control flow: link the partners */
        int me = s->ninst;
        if (op == OP_IF || op == OP_BGNLOOP) { if (sp == 32) return fail(&ps, "nesting too deep"); stack[sp++] = me; }
        else if (op == OP_ELSE) { if (!sp) return fail(&ps, "ELSE without IF"); s->inst[stack[sp - 1]].jump = (int16_t)(me + 1); stack[sp - 1] = me; }
        else if (op == OP_ENDIF) { if (!sp) return fail(&ps, "ENDIF without IF"); s->inst[stack[--sp]].jump = (int16_t)(me + 1); }
        else if (op == OP_ENDLOOP) { if (!sp) return fail(&ps, "ENDLOOP without BGNLOOP"); int b = stack[--sp]; s->inst[b].jump = (int16_t)(me + 1); in->jump = (int16_t)(b + 1); }
        s->ninst++;
        next_line(&ps);
    }
    if (sp) { snprintf(err, (size_t)errsize, "tgsi: unterminated IF/BGNLOOP"); return -1; }
    return 0;
}

void tgsi_free(struct tgsi_shader *s)
{
    free(s->inst);
    s->inst = NULL;
}
