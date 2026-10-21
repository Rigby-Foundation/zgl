/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* The GLSL preprocessor and lexer: source text in, the token array out.
 *
 * It is the C preprocessor GLSL describes: object- and function-like macros
 * (with ## pasting), #if/#ifdef/#ifndef/#elif/#else/#endif with integer
 * expressions and defined(), #undef, #error, #line, #version, #extension and
 * #pragma (accepted, nothing to do). Lines are read one at a time; the
 * tokens of the lines between two directives are macro-expanded together,
 * so a macro call may span lines, and a directive takes effect from the next
 * line on. */
#include "glsl_int.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

enum pk { PK_IDENT, PK_NUM, PK_PUNCT };

struct ptok { enum pk k; const char *s; int line; int noexpand; };
struct plist { struct ptok *t; int n, cap; };

struct macro {
    const char *name;
    int nparams;                /* -1: object-like */
    const char *params[32];
    struct ptok *body; int nbody;
    int disabled;               /* being expanded (no recursion) */
    struct macro *next;
};

struct pp {
    struct glc *c;
    struct macro *macros;
    struct plist pending;       /* raw tokens since the last directive */
    struct { int active, taken, else_seen; } cond[64];
    int ncond;
    int line;
    int saw_code;               /* a token or directive before #version */
};

static void pl_push(struct glc *c, struct plist *l, struct ptok t)
{
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 256;
        struct ptok *n = ar_alloc(c, (size_t)cap * sizeof *n);
        if (l->n) memcpy(n, l->t, (size_t)l->n * sizeof *n);
        l->t = n; l->cap = cap;
    }
    l->t[l->n++] = t;
}

static int active(struct pp *p) { return p->ncond == 0 || p->cond[p->ncond - 1].active; }

static struct macro *find_macro(struct pp *p, const char *name)
{
    for (struct macro *m = p->macros; m; m = m->next) if (!strcmp(m->name, name)) return m;
    return NULL;
}

static void undef(struct pp *p, const char *name)
{
    for (struct macro **m = &p->macros; *m; m = &(*m)->next)
        if (!strcmp((*m)->name, name)) { *m = (*m)->next; return; }
}

/* ---- tokenizing one line ----------------------------------------------------------- */

static const char *const puncts[] = {
    "<<=", ">>=", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "^^",
    "+=", "-=", "*=", "/=", "%=", "&=", "^=", "|=", "##", NULL
};

static int ident_start(int ch) { return isalpha(ch) || ch == '_'; }
static int ident_char(int ch) { return isalnum(ch) || ch == '_'; }

/* the next raw token of a line, or 0 at its end */
static int lex_raw(struct pp *p, const char **cur, struct ptok *t)
{
    const char *s = *cur;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\f' || *s == '\v') s++;
    if (!*s) { *cur = s; return 0; }
    const char *b = s;
    if (ident_start((unsigned char)*s)) {
        while (ident_char((unsigned char)*s)) s++;
        t->k = PK_IDENT;
    } else if (isdigit((unsigned char)*s) || (*s == '.' && isdigit((unsigned char)s[1]))) {
        while (ident_char((unsigned char)*s) || *s == '.' ||
               ((*s == '+' || *s == '-') && (s[-1] == 'e' || s[-1] == 'E') && !(b[0] == '0' && (b[1] == 'x' || b[1] == 'X'))))
            s++;
        t->k = PK_NUM;
    } else {
        t->k = PK_PUNCT;
        int n = 1;
        for (int i = 0; puncts[i]; i++) { size_t l = strlen(puncts[i]); if (!strncmp(s, puncts[i], l)) { n = (int)l; break; } }
        s += n;
    }
    t->s = ar_strndup(p->c, b, (size_t)(s - b));
    t->line = p->line;
    t->noexpand = 0;
    *cur = s;
    return 1;
}

/* ---- macro expansion ----------------------------------------------------------------- */

static void expand(struct pp *p, const struct ptok *in, int n, struct plist *out);

static int param_index(const struct macro *m, const char *s)
{
    for (int i = 0; i < m->nparams; i++) if (!strcmp(m->params[i], s)) return i;
    return -1;
}

/* `in[*i]` names macro m: append its expansion to out, advance *i past the call */
static int expand_macro(struct pp *p, struct macro *m, const struct ptok *in, int n, int *i, struct plist *out)
{
    int at = *i + 1;
    struct plist args[32];
    int nargs = 0;
    if (m->nparams >= 0) {
        if (at >= n || strcmp(in[at].s, "(")) return 0;     /* the name alone is not a call */
        at++;
        memset(args, 0, sizeof args);
        int depth = 0;
        if (at < n && !strcmp(in[at].s, ")") && m->nparams == 0) { at++; }
        else {
            nargs = 1;
            for (;; at++) {
                if (at >= n) glc_error(p->c, "unterminated call of macro '%s'", m->name);
                const char *s = in[at].s;
                if (!strcmp(s, "(")) depth++;
                else if (!strcmp(s, ")")) { if (depth-- == 0) { at++; break; } }
                else if (!strcmp(s, ",") && depth == 0) { if (nargs++ >= 32) glc_error(p->c, "too many macro arguments"); continue; }
                pl_push(p->c, &args[nargs - 1], in[at]);
            }
        }
        if (nargs != m->nparams && !(m->nparams == 0 && nargs == 1 && args[0].n == 0))
            glc_error(p->c, "macro '%s' takes %d argument(s), %d given", m->name, m->nparams, nargs);
    }
    /* substitute: a parameter becomes its (fully expanded) argument, except next to ## */
    struct plist body = { 0 };
    for (int k = 0; k < m->nbody; k++) {
        const struct ptok *bt = &m->body[k];
        int pi = bt->k == PK_IDENT && m->nparams > 0 ? param_index(m, bt->s) : -1;
        int pasted = (k + 1 < m->nbody && !strcmp(m->body[k + 1].s, "##")) || (k > 0 && !strcmp(m->body[k - 1].s, "##"));
        if (pi >= 0) {
            if (pasted) for (int j = 0; j < args[pi].n; j++) pl_push(p->c, &body, args[pi].t[j]);
            else expand(p, args[pi].t, args[pi].n, &body);
        } else {
            struct ptok t = *bt;
            t.line = in[*i].line;
            pl_push(p->c, &body, t);
        }
    }
    /* ## joins its neighbours into one token */
    for (int k = 0; k < body.n; k++) {
        if (strcmp(body.t[k].s, "##") || body.t[k].k != PK_PUNCT) continue;
        if (k == 0 || k + 1 >= body.n) glc_error(p->c, "'##' at the edge of macro '%s'", m->name);
        size_t a = strlen(body.t[k - 1].s), b = strlen(body.t[k + 1].s);
        char *s = ar_alloc(p->c, a + b + 1);
        memcpy(s, body.t[k - 1].s, a); memcpy(s + a, body.t[k + 1].s, b + 1);
        body.t[k - 1].s = s;
        body.t[k - 1].k = ident_start((unsigned char)s[0]) ? PK_IDENT : isdigit((unsigned char)s[0]) ? PK_NUM : PK_PUNCT;
        memmove(&body.t[k], &body.t[k + 2], (size_t)(body.n - k - 2) * sizeof body.t[0]);
        body.n -= 2;
        k--;
    }
    m->disabled = 1;
    expand(p, body.t, body.n, out);
    m->disabled = 0;
    *i = at;
    return 1;
}

static void expand(struct pp *p, const struct ptok *in, int n, struct plist *out)
{
    for (int i = 0; i < n; ) {
        struct ptok t = in[i];
        if (t.k == PK_IDENT && !t.noexpand) {
            if (!strcmp(t.s, "__LINE__")) {
                char b[16]; snprintf(b, sizeof b, "%d", t.line);
                struct ptok l = { PK_NUM, ar_strdup(p->c, b), t.line, 0 };
                pl_push(p->c, out, l); i++; continue;
            }
            struct macro *m = find_macro(p, t.s);
            if (m && m->disabled) { t.noexpand = 1; pl_push(p->c, out, t); i++; continue; }
            if (m && expand_macro(p, m, in, n, &i, out)) continue;
        }
        pl_push(p->c, out, t);
        i++;
    }
}

/* ---- the final tokens ------------------------------------------------------------------ */

static void emit_tok(struct pp *p, const struct ptok *pt)
{
    struct glc *c = p->c;
    struct tok t;
    memset(&t, 0, sizeof t);
    t.s = pt->s;
    t.line = pt->line;
    if (pt->k == PK_IDENT) t.kind = TK_IDENT;
    else if (pt->k == PK_PUNCT) t.kind = TK_PUNCT;
    else {
        const char *s = pt->s;
        size_t len = strlen(s);
        int is_hex = s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
        int is_float = !is_hex && (strchr(s, '.') || strchr(s, 'e') || strchr(s, 'E') ||
                                   (len && (s[len - 1] == 'f' || s[len - 1] == 'F')));
        c->line = pt->line;
        if (is_float) {
            char *end;
            t.kind = TK_FLOAT;
            t.f = strtod(s, &end);
            if (*end == 'f' || *end == 'F') end++;
            else if ((end[0] == 'l' && end[1] == 'f') || (end[0] == 'L' && end[1] == 'F')) end += 2;
            if (*end) glc_error(c, "bad number '%s'", s);
        } else {
            char *end;
            unsigned long long v = strtoull(s, &end, is_hex ? 16 : (s[0] == '0' && s[1]) ? 8 : 10);
            t.kind = TK_INT;
            if (*end == 'u' || *end == 'U') { t.kind = TK_UINT; end++; }
            if (*end) glc_error(c, "bad number '%s'", s);
            if (v > 0xFFFFFFFFull) glc_error(c, "integer constant '%s' too large", s);
            t.u = (uint32_t)v;
        }
    }
    if (c->ntoks == c->captoks) {
        int cap = c->captoks ? c->captoks * 2 : 1024;
        struct tok *n = ar_alloc(c, (size_t)cap * sizeof *n);
        if (c->ntoks) memcpy(n, c->toks, (size_t)c->ntoks * sizeof *n);
        c->toks = n; c->captoks = cap;
    }
    c->toks[c->ntoks++] = t;
}

static void flush(struct pp *p)
{
    if (!p->pending.n) return;
    struct plist out = { 0 };
    expand(p, p->pending.t, p->pending.n, &out);
    for (int i = 0; i < out.n; i++) emit_tok(p, &out.t[i]);
    p->pending.n = 0;
}

/* ---- #if expressions --------------------------------------------------------------------- */

struct ev { struct pp *p; const struct ptok *t; int n, i; };

static long long ev_cond(struct ev *e);

static int ev_is(struct ev *e, const char *s) { return e->i < e->n && !strcmp(e->t[e->i].s, s); }

static long long ev_primary(struct ev *e)
{
    if (e->i >= e->n) glc_error(e->p->c, "#if: expression ends early");
    const struct ptok *t = &e->t[e->i++];
    if (!strcmp(t->s, "(")) { long long v = ev_cond(e); if (!ev_is(e, ")")) glc_error(e->p->c, "#if: ')' expected"); e->i++; return v; }
    if (!strcmp(t->s, "!")) return !ev_primary(e);
    if (!strcmp(t->s, "~")) return ~ev_primary(e);
    if (!strcmp(t->s, "-")) return -ev_primary(e);
    if (!strcmp(t->s, "+")) return ev_primary(e);
    if (t->k == PK_NUM) {
        char *end;
        const char *s = t->s;
        long long v = strtoll(s, &end, (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 16 : (s[0] == '0' && s[1]) ? 8 : 10);
        if (*end == 'u' || *end == 'U') end++;
        if (*end) glc_error(e->p->c, "#if: '%s' is not an integer", s);
        return v;
    }
    if (t->k == PK_IDENT) return 0;     /* an undefined name */
    glc_error(e->p->c, "#if: unexpected '%s'", t->s);
}

static int ev_prec(const char *op)
{
    static const struct { const char *op; int p; } tab[] = {
        { "*", 10 }, { "/", 10 }, { "%", 10 }, { "+", 9 }, { "-", 9 }, { "<<", 8 }, { ">>", 8 },
        { "<", 7 }, { ">", 7 }, { "<=", 7 }, { ">=", 7 }, { "==", 6 }, { "!=", 6 },
        { "&", 5 }, { "^", 4 }, { "|", 3 }, { "&&", 2 }, { "||", 1 },
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++) if (!strcmp(tab[i].op, op)) return tab[i].p;
    return -1;
}

static long long ev_binary(struct ev *e, int min)
{
    long long a = ev_primary(e);
    for (;;) {
        if (e->i >= e->n) break;
        const char *op = e->t[e->i].s;
        int pr = ev_prec(op);
        if (pr < 0 || pr < min) break;
        e->i++;
        long long b = ev_binary(e, pr + 1);
        if ((!strcmp(op, "/") || !strcmp(op, "%")) && b == 0) glc_error(e->p->c, "#if: division by zero");
        switch (op[0]) {
        case '*': a = a * b; break;
        case '/': a = a / b; break;
        case '%': a = a % b; break;
        case '+': a = a + b; break;
        case '-': a = a - b; break;
        case '<': a = op[1] == '<' ? a << b : op[1] == '=' ? a <= b : a < b; break;
        case '>': a = op[1] == '>' ? a >> b : op[1] == '=' ? a >= b : a > b; break;
        case '=': a = a == b; break;
        case '!': a = a != b; break;
        case '&': a = op[1] == '&' ? (a && b) : (a & b); break;
        case '^': a = a ^ b; break;
        case '|': a = op[1] == '|' ? (a || b) : (a | b); break;
        }
    }
    return a;
}

static long long ev_cond(struct ev *e)
{
    long long c = ev_binary(e, 1);
    if (!ev_is(e, "?")) return c;
    e->i++;
    long long a = ev_cond(e);
    if (!ev_is(e, ":")) glc_error(e->p->c, "#if: ':' expected");
    e->i++;
    long long b = ev_cond(e);
    return c ? a : b;
}

static int eval_if(struct pp *p, const char *rest)
{
    /* defined X / defined(X) first, then expand, then evaluate */
    struct plist raw = { 0 }, pre = { 0 }, out = { 0 };
    struct ptok t;
    const char *cur = rest;
    while (lex_raw(p, &cur, &t)) pl_push(p->c, &raw, t);
    for (int i = 0; i < raw.n; i++) {
        if (raw.t[i].k == PK_IDENT && !strcmp(raw.t[i].s, "defined")) {
            int paren = i + 1 < raw.n && !strcmp(raw.t[i + 1].s, "(");
            int ni = i + 1 + paren;
            if (ni >= raw.n || raw.t[ni].k != PK_IDENT) glc_error(p->c, "#if: defined needs a name");
            struct ptok v = { PK_NUM, find_macro(p, raw.t[ni].s) ? "1" : "0", raw.t[i].line, 0 };
            pl_push(p->c, &pre, v);
            i = ni + paren;
            continue;
        }
        pl_push(p->c, &pre, raw.t[i]);
    }
    expand(p, pre.t, pre.n, &out);
    struct ev e = { p, out.t, out.n, 0 };
    long long v = ev_cond(&e);
    if (e.i != e.n) glc_error(p->c, "#if: junk after the expression ('%s')", out.t[e.i].s);
    return v != 0;
}

/* ---- directives ------------------------------------------------------------------------ */

static void define_macro(struct pp *p, const char *rest)
{
    const char *cur = rest;
    struct ptok name;
    if (!lex_raw(p, &cur, &name) || name.k != PK_IDENT) glc_error(p->c, "#define needs a name");
    if (!strncmp(name.s, "GL_", 3) || strstr(name.s, "__")) {
        /* reserved names: allowed to redefine our own predefined ones quietly */
    }
    struct macro *m = ar_alloc(p->c, sizeof *m);
    m->name = name.s;
    m->nparams = -1;
    if (*cur == '(') {                  /* no space: a function-like macro */
        cur++;
        m->nparams = 0;
        struct ptok t;
        for (;;) {
            if (!lex_raw(p, &cur, &t)) glc_error(p->c, "#define %s: unterminated parameter list", m->name);
            if (!strcmp(t.s, ")") && m->nparams == 0) break;
            if (t.k != PK_IDENT) glc_error(p->c, "#define %s: parameter name expected", m->name);
            if (m->nparams >= 32) glc_error(p->c, "#define %s: too many parameters", m->name);
            m->params[m->nparams++] = t.s;
            if (!lex_raw(p, &cur, &t)) glc_error(p->c, "#define %s: unterminated parameter list", m->name);
            if (!strcmp(t.s, ")")) break;
            if (strcmp(t.s, ",")) glc_error(p->c, "#define %s: ',' expected", m->name);
        }
    }
    struct plist body = { 0 };
    struct ptok t;
    while (lex_raw(p, &cur, &t)) pl_push(p->c, &body, t);
    m->body = body.t; m->nbody = body.n;
    undef(p, m->name);
    m->next = p->macros;
    p->macros = m;
}

static void predefine(struct pp *p, const char *name, const char *value)
{
    char line[160];
    snprintf(line, sizeof line, "%s %s", name, value);
    define_macro(p, line);
}

static void set_version(struct pp *p, const char *rest)
{
    struct glc *c = p->c;
    if (p->saw_code) glc_error(c, "#version must come first");
    int v = atoi(rest);
    const char *prof = rest;
    while (*prof && (isdigit((unsigned char)*prof) || *prof == ' ' || *prof == '\t')) prof++;
    switch (v) {
    case 100: case 110: case 120: case 130: case 140: case 150: case 300: case 330: break;
    default: glc_error(c, "GLSL version %d is not supported (100, 110-150, 300 es, 330)", v);
    }
    c->version = v;
    c->es = v == 100 || v == 300 || !strncmp(prof, "es", 2);
    if (c->es && v != 100 && v != 300) glc_error(c, "#version %d es is not an ES version", v);
    char b[16];
    snprintf(b, sizeof b, "%d", v);
    predefine(p, "__VERSION__", b);
    if (c->es) { predefine(p, "GL_ES", "1"); predefine(p, "GL_FRAGMENT_PRECISION_HIGH", "1"); }
    else if (v >= 150 && strncmp(prof, "compatibility", 13)) predefine(p, "GL_core_profile", "1");
    if (!strncmp(prof, "compatibility", 13)) predefine(p, "GL_compatibility_profile", "1");
}

static void directive(struct pp *p, const char *line)
{
    struct glc *c = p->c;
    const char *s = line + 1;
    while (*s == ' ' || *s == '\t') s++;
    const char *w = s;
    while (ident_char((unsigned char)*s)) s++;
    size_t wl = (size_t)(s - w);
    const char *rest = s;
#define IS(x) (wl == sizeof(x) - 1 && !strncmp(w, x, wl))
    c->line = p->line;
    if (IS("if") || IS("ifdef") || IS("ifndef")) {
        if (p->ncond >= 64) glc_error(c, "#if nested too deep");
        int on = 0;
        if (active(p)) {
            if (IS("if")) on = eval_if(p, rest);
            else {
                struct ptok n; const char *cur = rest;
                if (!lex_raw(p, &cur, &n) || n.k != PK_IDENT) glc_error(c, "#%.*s needs a name", (int)wl, w);
                on = (find_macro(p, n.s) != NULL) == IS("ifdef");
            }
        }
        p->cond[p->ncond].active = active(p) && on;
        p->cond[p->ncond].taken = on || !active(p);      /* an inactive parent: nothing here runs */
        p->cond[p->ncond].else_seen = 0;
        p->ncond++;
        return;
    }
    if (IS("elif")) {
        if (!p->ncond || p->cond[p->ncond - 1].else_seen) glc_error(c, "#elif without #if");
        p->ncond--;
        int parent = active(p);
        p->ncond++;
        int on = parent && !p->cond[p->ncond - 1].taken && eval_if(p, rest);
        p->cond[p->ncond - 1].active = on;
        if (on) p->cond[p->ncond - 1].taken = 1;
        return;
    }
    if (IS("else")) {
        if (!p->ncond || p->cond[p->ncond - 1].else_seen) glc_error(c, "#else without #if");
        p->ncond--;
        int parent = active(p);
        p->ncond++;
        p->cond[p->ncond - 1].active = parent && !p->cond[p->ncond - 1].taken;
        p->cond[p->ncond - 1].taken = 1;
        p->cond[p->ncond - 1].else_seen = 1;
        return;
    }
    if (IS("endif")) {
        if (!p->ncond) glc_error(c, "#endif without #if");
        p->ncond--;
        return;
    }
    if (!active(p)) return;
    if (wl == 0) return;                                   /* a lone # */
    if (IS("version")) { set_version(p, rest); return; }
    p->saw_code = 1;
    if (IS("define")) { define_macro(p, rest); return; }
    if (IS("undef")) {
        struct ptok n; const char *cur = rest;
        if (!lex_raw(p, &cur, &n) || n.k != PK_IDENT) glc_error(c, "#undef needs a name");
        undef(p, n.s);
        return;
    }
    if (IS("error")) { while (*rest == ' ' || *rest == '\t') rest++; glc_error(c, "#error %s", rest); }
    if (IS("line")) { int l = atoi(rest); if (l > 0) p->line = l - 1; return; }
    if (IS("pragma") || IS("extension")) return;
    glc_error(c, "unknown directive #%.*s", (int)wl, w);
#undef IS
}

/* ---- the driver ----------------------------------------------------------------------- */

void pp_run(struct glc *c, const char *src)
{
    struct pp p;
    memset(&p, 0, sizeof p);
    p.c = c;
    c->version = 110;
    /* comments become spaces (newlines kept); continuation lines are joined */
    size_t len = strlen(src);
    char *buf = ar_alloc(c, len + 2);
    size_t o = 0;
    for (size_t i = 0; i < len; ) {
        if (src[i] == '/' && src[i + 1] == '/') { while (i < len && src[i] != '\n') i++; continue; }
        if (src[i] == '/' && src[i + 1] == '*') {
            i += 2;
            while (i < len && !(src[i] == '*' && src[i + 1] == '/')) { if (src[i] == '\n') buf[o++] = '\n'; i++; }
            if (i >= len) glc_error(c, "unterminated comment");
            i += 2;
            buf[o++] = ' ';
            continue;
        }
        buf[o++] = src[i++];
    }
    buf[o] = 0;
    predefine(&p, "GL_ARB_explicit_attrib_location", "1");
    predefine(&p, "GL_ARB_shader_bit_encoding", "1");
    predefine(&p, "GL_ARB_texture_rectangle", "1");
    char *line = buf;
    char *joined = ar_alloc(c, o + 2);
    while (line && *line) {
        /* gather one logical line (backslash-newline continues it) */
        char *eol = line;
        size_t jn = 0;
        int extra = 0;
        for (;;) {
            char *nl = strchr(eol, '\n');
            size_t n = nl ? (size_t)(nl - eol) : strlen(eol);
            if (n && eol[n - 1] == '\r') n--;
            if (n && eol[n - 1] == '\\') { memcpy(joined + jn, eol, n - 1); jn += n - 1; extra++; eol = nl ? nl + 1 : eol + n; if (!nl) break; continue; }
            memcpy(joined + jn, eol, n); jn += n;
            eol = nl ? nl + 1 : NULL;
            break;
        }
        joined[jn] = 0;
        p.line++;
        const char *s = joined;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#') {
            flush(&p);
            directive(&p, s);
        } else if (active(&p)) {
            struct ptok t;
            const char *cur = s;
            while (lex_raw(&p, &cur, &t)) { pl_push(c, &p.pending, t); p.saw_code = 1; }
        }
        p.line += extra;
        line = eol;
    }
    flush(&p);
    if (p.ncond) glc_error(c, "#if without #endif");
    struct ptok eof = { PK_PUNCT, "", p.line, 0 };
    emit_tok(&p, &eof);
    c->toks[c->ntoks - 1].kind = TK_EOF;
}
