/*
 * betl-ssisexpr — SSIS Expression Language engine.
 *
 * v0.1 scope: literals, column refs (bare + bracketed), arithmetic /
 * comparison / logical / ternary operators with three-valued logic,
 * string concat via `+`, explicit casts. No functions yet.
 *
 * Stages:
 *   compile() — lex, parse, resolve column refs against input_schema,
 *               return an Ast handle.
 *   evaluate() — per-row tree walk; staged into the LmCol builder;
 *               handed back to the host as an Arrow leaf.
 *
 * All allocation lives in compile-time and per-batch arenas; no per-row
 * malloc beyond growing the LmCol's string buffer. String values during
 * evaluation point into either the input batch (zero-copy) or a per-
 * batch scratch arena (owned by the engine, freed at evaluate exit).
 */

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>           /* strcasecmp */

#include "betl/provider.h"


/* ============================================================== *
 *  AST                                                             *
 * ============================================================== */

typedef enum {
    N_LIT_INT,
    N_LIT_FLOAT,
    N_LIT_STR,
    N_LIT_BOOL,
    N_LIT_NULL,        /* NULL(DT_xx) — typed null */
    N_COLREF,
    N_UNARY,           /* -x  !x  ~x */
    N_BINOP,
    N_TERNARY,
    N_CAST,
} NodeKind;

typedef enum {
    /* Cast / typed-null target types — what SSIS calls DT_*. We map
     * onto betl's value kinds at evaluate time. */
    DT_I1,
    DT_I2,
    DT_I4,
    DT_I8,
    DT_R4,
    DT_R8,
    DT_BOOL,
    DT_WSTR,
    DT_STR,
} SsisDt;

typedef enum {
    OP_NEG, OP_NOT, OP_BNOT,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE,
    OP_AND, OP_OR,
} Op;

typedef struct Node Node;
struct Node {
    NodeKind kind;
    union {
        int64_t      i64;
        double       f64;
        struct { char *s; size_t n; } str;
        int          b;
        struct { SsisDt dt; } null_typed;
        struct { size_t col_idx; } colref;
        struct { Op op; Node *a; } unary;
        struct { Op op; Node *a; Node *b; } binop;
        struct { Node *cond; Node *t; Node *f; } ternary;
        struct { SsisDt dt; int has_len; int64_t len; Node *a; } cast;
    };
};

/* Arena-style node allocator. compile() owns it for the engine's lifetime. */
typedef struct {
    Node  **nodes;
    size_t  count;
    size_t  cap;
} Arena;

static Node *arena_new(Arena *a, NodeKind k) {
    if (a->count == a->cap) {
        size_t nc = a->cap ? a->cap * 2 : 16;
        Node **nn = realloc(a->nodes, nc * sizeof *nn);
        if (!nn) return NULL;
        a->nodes = nn; a->cap = nc;
    }
    Node *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->kind = k;
    a->nodes[a->count++] = n;
    return n;
}

static void arena_free(Arena *a) {
    for (size_t i = 0; i < a->count; ++i) {
        Node *n = a->nodes[i];
        if (n->kind == N_LIT_STR) free(n->str.s);
        free(n);
    }
    free(a->nodes);
    a->nodes = NULL; a->count = a->cap = 0;
}


/* ============================================================== *
 *  Lexer                                                           *
 * ============================================================== */

typedef enum {
    T_EOF,
    T_INT, T_FLOAT, T_STR, T_IDENT, T_BR_IDENT,
    T_LPAREN, T_RPAREN, T_COMMA,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT,
    T_BANG, T_TILDE,
    T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE,
    T_AMPAMP, T_PIPEPIPE,
    T_QMARK, T_COLON,
    T_KW_TRUE, T_KW_FALSE, T_KW_NULL,
} TokKind;

typedef struct {
    TokKind  kind;
    int64_t  i64;        /* for T_INT */
    double   f64;        /* for T_FLOAT */
    char    *str;        /* decoded payload for T_STR / IDENT (heap, NUL-terminated) */
} Tok;

typedef struct {
    const char *p;
    Tok        *toks;
    size_t      n_toks;
    size_t      cap_toks;
    char        err[256];
} Lex;

static int lex_err(Lex *L, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(L->err, sizeof L->err, fmt, ap);
    va_end(ap);
    return -1;
}

static int lex_push(Lex *L, Tok t) {
    if (L->n_toks == L->cap_toks) {
        size_t nc = L->cap_toks ? L->cap_toks * 2 : 32;
        Tok *nt = realloc(L->toks, nc * sizeof *nt);
        if (!nt) return -1;
        L->toks = nt; L->cap_toks = nc;
    }
    L->toks[L->n_toks++] = t;
    return 0;
}

static int strieq(const char *a, size_t na, const char *b) {
    size_t nb = strlen(b);
    if (na != nb) return 0;
    for (size_t i = 0; i < na; ++i) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    }
    return 1;
}

/* Decode a C-style escape sequence at p (pointing past the backslash).
 * Writes the decoded byte to *out and advances *pp. Returns 0/-1. */
static int decode_escape(const char **pp, char *out) {
    const char *p = *pp;
    if (!*p) return -1;
    switch (*p) {
        case 'n':  *out = '\n'; break;
        case 't':  *out = '\t'; break;
        case 'r':  *out = '\r'; break;
        case '\\': *out = '\\'; break;
        case '"':  *out = '"';  break;
        case '0':  *out = '\0'; break;
        default:   return -1;
    }
    ++p;
    *pp = p;
    return 0;
}

static int lex_string(Lex *L) {
    ++L->p;                                /* skip opening " */
    size_t cap = 16, n = 0;
    char *buf = malloc(cap);
    if (!buf) return -1;
    while (*L->p && *L->p != '"') {
        char c;
        if (*L->p == '\\') {
            ++L->p;
            if (decode_escape(&L->p, &c) != 0) {
                free(buf);
                return lex_err(L, "invalid escape in string literal");
            }
        } else {
            c = *L->p++;
        }
        if (n + 1 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); return -1; }
            buf = nb;
        }
        buf[n++] = c;
    }
    if (*L->p != '"') {
        free(buf);
        return lex_err(L, "unterminated string literal");
    }
    buf[n] = '\0';
    ++L->p;                                 /* closing " */
    Tok t = { .kind = T_STR, .str = buf };
    int rc = lex_push(L, t);
    if (rc != 0) free(buf);
    return rc;
}

static int lex_number(Lex *L) {
    const char *start = L->p;
    int is_float = 0;
    /* hex */
    if (L->p[0] == '0' && (L->p[1] == 'x' || L->p[1] == 'X')) {
        L->p += 2;
        const char *digits = L->p;
        while (isxdigit((unsigned char)*L->p)) ++L->p;
        if (L->p == digits) return lex_err(L, "malformed hex literal");
        char tmp[32];
        size_t n = (size_t)(L->p - digits);
        if (n >= sizeof tmp) return lex_err(L, "hex literal too long");
        memcpy(tmp, digits, n); tmp[n] = '\0';
        Tok t = { .kind = T_INT, .i64 = (int64_t)strtoll(tmp, NULL, 16) };
        return lex_push(L, t);
    }
    while (isdigit((unsigned char)*L->p)) ++L->p;
    if (*L->p == '.') {
        is_float = 1; ++L->p;
        while (isdigit((unsigned char)*L->p)) ++L->p;
    }
    if (*L->p == 'e' || *L->p == 'E') {
        is_float = 1; ++L->p;
        if (*L->p == '+' || *L->p == '-') ++L->p;
        while (isdigit((unsigned char)*L->p)) ++L->p;
    }
    char tmp[64];
    size_t n = (size_t)(L->p - start);
    if (n >= sizeof tmp) return lex_err(L, "numeric literal too long");
    memcpy(tmp, start, n); tmp[n] = '\0';
    Tok t = { 0 };
    if (is_float) {
        char *end = NULL; errno = 0;
        double v = strtod(tmp, &end);
        if (end == tmp || errno != 0) return lex_err(L, "malformed float literal");
        t.kind = T_FLOAT; t.f64 = v;
    } else {
        char *end = NULL; errno = 0;
        long long v = strtoll(tmp, &end, 10);
        if (end == tmp || errno != 0) return lex_err(L, "malformed integer literal");
        t.kind = T_INT; t.i64 = (int64_t)v;
    }
    return lex_push(L, t);
}

static int lex_ident(Lex *L) {
    const char *start = L->p;
    while (isalnum((unsigned char)*L->p) || *L->p == '_') ++L->p;
    size_t n = (size_t)(L->p - start);
    Tok t = { 0 };
    if (strieq(start, n, "TRUE"))       t.kind = T_KW_TRUE;
    else if (strieq(start, n, "FALSE")) t.kind = T_KW_FALSE;
    else if (strieq(start, n, "NULL"))  t.kind = T_KW_NULL;
    else                                t.kind = T_IDENT;
    char *cp = malloc(n + 1);
    if (!cp) return -1;
    memcpy(cp, start, n); cp[n] = '\0';
    t.str = cp;
    int rc = lex_push(L, t);
    if (rc != 0) free(cp);
    return rc;
}

static int lex_bracket_ident(Lex *L) {
    ++L->p;                                /* skip [ */
    const char *start = L->p;
    while (*L->p && *L->p != ']') ++L->p;
    if (*L->p != ']') return lex_err(L, "unterminated bracketed identifier");
    size_t n = (size_t)(L->p - start);
    char *cp = malloc(n + 1);
    if (!cp) return -1;
    memcpy(cp, start, n); cp[n] = '\0';
    ++L->p;                                /* closing ] */
    Tok t = { .kind = T_BR_IDENT, .str = cp };
    int rc = lex_push(L, t);
    if (rc != 0) free(cp);
    return rc;
}

static int lex_all(Lex *L) {
    while (*L->p) {
        unsigned char c = (unsigned char)*L->p;
        if (isspace(c)) { ++L->p; continue; }
        if (isdigit(c)) { if (lex_number(L) != 0) return -1; continue; }
        if (c == '"')   { if (lex_string(L) != 0) return -1; continue; }
        if (c == '[')   { if (lex_bracket_ident(L) != 0) return -1; continue; }
        if (isalpha(c) || c == '_') { if (lex_ident(L) != 0) return -1; continue; }

        Tok t = { 0 };
        switch (c) {
            case '(': t.kind = T_LPAREN;   ++L->p; break;
            case ')': t.kind = T_RPAREN;   ++L->p; break;
            case ',': t.kind = T_COMMA;    ++L->p; break;
            case '+': t.kind = T_PLUS;     ++L->p; break;
            case '-': t.kind = T_MINUS;    ++L->p; break;
            case '*': t.kind = T_STAR;     ++L->p; break;
            case '/': t.kind = T_SLASH;    ++L->p; break;
            case '%': t.kind = T_PERCENT;  ++L->p; break;
            case '~': t.kind = T_TILDE;    ++L->p; break;
            case '?': t.kind = T_QMARK;    ++L->p; break;
            case ':': t.kind = T_COLON;    ++L->p; break;
            case '!':
                if (L->p[1] == '=') { t.kind = T_NE; L->p += 2; }
                else                { t.kind = T_BANG; ++L->p; }
                break;
            case '=':
                if (L->p[1] == '=') { t.kind = T_EQ; L->p += 2; }
                else                { return lex_err(L, "unexpected '=' (did you mean '==' ?)"); }
                break;
            case '<':
                if (L->p[1] == '=') { t.kind = T_LE; L->p += 2; }
                else                { t.kind = T_LT; ++L->p; }
                break;
            case '>':
                if (L->p[1] == '=') { t.kind = T_GE; L->p += 2; }
                else                { t.kind = T_GT; ++L->p; }
                break;
            case '&':
                if (L->p[1] == '&') { t.kind = T_AMPAMP;   L->p += 2; }
                else                { return lex_err(L, "bitwise & not supported in v0.1"); }
                break;
            case '|':
                if (L->p[1] == '|') { t.kind = T_PIPEPIPE; L->p += 2; }
                else                { return lex_err(L, "bitwise | not supported in v0.1"); }
                break;
            default:
                return lex_err(L, "unexpected character '%c'", (int)c);
        }
        if (lex_push(L, t) != 0) return -1;
    }
    Tok eof = { .kind = T_EOF };
    return lex_push(L, eof);
}

static void lex_free(Lex *L) {
    for (size_t i = 0; i < L->n_toks; ++i) free(L->toks[i].str);
    free(L->toks);
}


/* ============================================================== *
 *  Parser (Pratt)                                                  *
 * ============================================================== */

typedef struct {
    Tok        *toks;
    size_t      pos;
    Arena      *arena;
    /* Column-name → index resolution table. */
    char      **col_names;
    char       *col_fmts;        /* parallel: 'l'/'g'/'u'/'b' */
    size_t      n_cols;
    char        err[256];
} Par;

static int par_err(Par *P, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(P->err, sizeof P->err, fmt, ap);
    va_end(ap);
    return -1;
}

static Tok *peek(Par *P)             { return &P->toks[P->pos]; }
static Tok *advance(Par *P)          { return &P->toks[P->pos++]; }
static int  check(Par *P, TokKind k) { return peek(P)->kind == k; }
static int  match(Par *P, TokKind k) { if (check(P, k)) { ++P->pos; return 1; } return 0; }

/* Forward decls — Pratt levels. */
static Node *p_ternary(Par *P);
static Node *p_or(Par *P);
static Node *p_and(Par *P);
static Node *p_eq(Par *P);
static Node *p_rel(Par *P);
static Node *p_add(Par *P);
static Node *p_mul(Par *P);
static Node *p_unary(Par *P);
static Node *p_primary(Par *P);

/* Recognize a SSIS DT_xxx type name. Returns 1 + writes *out on hit. */
static int parse_dt(const char *name, SsisDt *out) {
    if      (strcasecmp(name, "DT_I1")   == 0) { *out = DT_I1;   return 1; }
    else if (strcasecmp(name, "DT_I2")   == 0) { *out = DT_I2;   return 1; }
    else if (strcasecmp(name, "DT_I4")   == 0) { *out = DT_I4;   return 1; }
    else if (strcasecmp(name, "DT_I8")   == 0) { *out = DT_I8;   return 1; }
    else if (strcasecmp(name, "DT_R4")   == 0) { *out = DT_R4;   return 1; }
    else if (strcasecmp(name, "DT_R8")   == 0) { *out = DT_R8;   return 1; }
    else if (strcasecmp(name, "DT_BOOL") == 0) { *out = DT_BOOL; return 1; }
    else if (strcasecmp(name, "DT_WSTR") == 0) { *out = DT_WSTR; return 1; }
    else if (strcasecmp(name, "DT_STR")  == 0) { *out = DT_STR;  return 1; }
    return 0;
}

/* Resolve a column name → index, case-insensitive (SSIS is case-insensitive). */
static int resolve_col(Par *P, const char *name, size_t *out_idx) {
    for (size_t i = 0; i < P->n_cols; ++i) {
        if (strcasecmp(name, P->col_names[i]) == 0) { *out_idx = i; return 0; }
    }
    return -1;
}

static Node *p_ternary(Par *P) {
    Node *c = p_or(P);
    if (!c) return NULL;
    if (match(P, T_QMARK)) {
        Node *t = p_ternary(P);
        if (!t) return NULL;
        if (!match(P, T_COLON)) { par_err(P, "expected ':' in ternary"); return NULL; }
        Node *f = p_ternary(P);
        if (!f) return NULL;
        Node *n = arena_new(P->arena, N_TERNARY);
        if (!n) return NULL;
        n->ternary.cond = c; n->ternary.t = t; n->ternary.f = f;
        return n;
    }
    return c;
}

static Node *mk_binop(Par *P, Op op, Node *a, Node *b) {
    Node *n = arena_new(P->arena, N_BINOP);
    if (!n) return NULL;
    n->binop.op = op; n->binop.a = a; n->binop.b = b;
    return n;
}

static Node *p_or(Par *P) {
    Node *a = p_and(P);
    while (a && check(P, T_PIPEPIPE)) {
        advance(P);
        Node *b = p_and(P);
        if (!b) return NULL;
        a = mk_binop(P, OP_OR, a, b);
    }
    return a;
}

static Node *p_and(Par *P) {
    Node *a = p_eq(P);
    while (a && check(P, T_AMPAMP)) {
        advance(P);
        Node *b = p_eq(P);
        if (!b) return NULL;
        a = mk_binop(P, OP_AND, a, b);
    }
    return a;
}

static Node *p_eq(Par *P) {
    Node *a = p_rel(P);
    while (a) {
        Op op;
        if (check(P, T_EQ)) op = OP_EQ;
        else if (check(P, T_NE)) op = OP_NE;
        else break;
        advance(P);
        Node *b = p_rel(P);
        if (!b) return NULL;
        a = mk_binop(P, op, a, b);
    }
    return a;
}

static Node *p_rel(Par *P) {
    Node *a = p_add(P);
    while (a) {
        Op op;
        if      (check(P, T_LT)) op = OP_LT;
        else if (check(P, T_LE)) op = OP_LE;
        else if (check(P, T_GT)) op = OP_GT;
        else if (check(P, T_GE)) op = OP_GE;
        else break;
        advance(P);
        Node *b = p_add(P);
        if (!b) return NULL;
        a = mk_binop(P, op, a, b);
    }
    return a;
}

static Node *p_add(Par *P) {
    Node *a = p_mul(P);
    while (a) {
        Op op;
        if      (check(P, T_PLUS))  op = OP_ADD;
        else if (check(P, T_MINUS)) op = OP_SUB;
        else break;
        advance(P);
        Node *b = p_mul(P);
        if (!b) return NULL;
        a = mk_binop(P, op, a, b);
    }
    return a;
}

static Node *p_mul(Par *P) {
    Node *a = p_unary(P);
    while (a) {
        Op op;
        if      (check(P, T_STAR))    op = OP_MUL;
        else if (check(P, T_SLASH))   op = OP_DIV;
        else if (check(P, T_PERCENT)) op = OP_MOD;
        else break;
        advance(P);
        Node *b = p_unary(P);
        if (!b) return NULL;
        a = mk_binop(P, op, a, b);
    }
    return a;
}

static Node *p_unary(Par *P) {
    if (match(P, T_MINUS)) {
        Node *a = p_unary(P);
        if (!a) return NULL;
        Node *n = arena_new(P->arena, N_UNARY);
        if (!n) return NULL;
        n->unary.op = OP_NEG; n->unary.a = a;
        return n;
    }
    if (match(P, T_BANG)) {
        Node *a = p_unary(P);
        if (!a) return NULL;
        Node *n = arena_new(P->arena, N_UNARY);
        if (!n) return NULL;
        n->unary.op = OP_NOT; n->unary.a = a;
        return n;
    }
    if (match(P, T_TILDE)) {
        Node *a = p_unary(P);
        if (!a) return NULL;
        Node *n = arena_new(P->arena, N_UNARY);
        if (!n) return NULL;
        n->unary.op = OP_BNOT; n->unary.a = a;
        return n;
    }
    return p_primary(P);
}

/* (DT_xx) expr  or  (DT_WSTR, N) expr  or  (expr).
 * Distinguish cast from parenthesized expression by peeking at IDENT-then-
 * (RPAREN | COMMA). */
static Node *p_paren_or_cast(Par *P) {
    /* We're positioned at LPAREN. */
    advance(P);                                /* consume ( */
    /* Look ahead one token: IDENT followed by ',' or ')' → cast. */
    if (peek(P)->kind == T_IDENT) {
        SsisDt dt;
        if (parse_dt(peek(P)->str, &dt)) {
            TokKind next = P->toks[P->pos + 1].kind;
            if (next == T_RPAREN || next == T_COMMA) {
                advance(P);                    /* consume DT_xxx */
                int has_len = 0; int64_t length = 0;
                if (match(P, T_COMMA)) {
                    if (!check(P, T_INT)) {
                        par_err(P, "cast length must be an integer literal");
                        return NULL;
                    }
                    length = peek(P)->i64;
                    advance(P);
                    has_len = 1;
                }
                if (!match(P, T_RPAREN)) {
                    par_err(P, "expected ')' after cast type");
                    return NULL;
                }
                Node *inner = p_unary(P);
                if (!inner) return NULL;
                Node *n = arena_new(P->arena, N_CAST);
                if (!n) return NULL;
                n->cast.dt = dt;
                n->cast.has_len = has_len;
                n->cast.len = length;
                n->cast.a = inner;
                return n;
            }
        }
    }
    /* Regular parenthesized expression. */
    Node *inner = p_ternary(P);
    if (!inner) return NULL;
    if (!match(P, T_RPAREN)) {
        par_err(P, "expected ')'");
        return NULL;
    }
    return inner;
}

static Node *p_primary(Par *P) {
    Tok *t = peek(P);
    switch (t->kind) {
        case T_INT: {
            advance(P);
            Node *n = arena_new(P->arena, N_LIT_INT);
            if (!n) return NULL;
            n->i64 = t->i64;
            return n;
        }
        case T_FLOAT: {
            advance(P);
            Node *n = arena_new(P->arena, N_LIT_FLOAT);
            if (!n) return NULL;
            n->f64 = t->f64;
            return n;
        }
        case T_STR: {
            advance(P);
            Node *n = arena_new(P->arena, N_LIT_STR);
            if (!n) return NULL;
            size_t blen = strlen(t->str);
            n->str.s = malloc(blen + 1);
            if (!n->str.s) return NULL;
            memcpy(n->str.s, t->str, blen + 1);
            n->str.n = blen;
            return n;
        }
        case T_KW_TRUE: case T_KW_FALSE: {
            int v = (t->kind == T_KW_TRUE);
            advance(P);
            Node *n = arena_new(P->arena, N_LIT_BOOL);
            if (!n) return NULL;
            n->b = v;
            return n;
        }
        case T_KW_NULL: {
            advance(P);
            if (!match(P, T_LPAREN)) {
                par_err(P, "NULL must be typed: NULL(DT_xxx)");
                return NULL;
            }
            if (peek(P)->kind != T_IDENT) {
                par_err(P, "NULL(DT_xxx) requires a type name");
                return NULL;
            }
            SsisDt dt;
            if (!parse_dt(peek(P)->str, &dt)) {
                par_err(P, "unknown DT type in NULL(%s)", peek(P)->str);
                return NULL;
            }
            advance(P);
            /* Optional (DT_WSTR, N) — accept and ignore the length. */
            if (match(P, T_COMMA)) {
                if (!check(P, T_INT)) { par_err(P, "NULL(...) length must be integer"); return NULL; }
                advance(P);
            }
            if (!match(P, T_RPAREN)) { par_err(P, "expected ')' after NULL(...)"); return NULL; }
            Node *n = arena_new(P->arena, N_LIT_NULL);
            if (!n) return NULL;
            n->null_typed.dt = dt;
            return n;
        }
        case T_IDENT:
        case T_BR_IDENT: {
            advance(P);
            size_t idx;
            if (resolve_col(P, t->str, &idx) != 0) {
                par_err(P, "unknown column '%s'", t->str);
                return NULL;
            }
            Node *n = arena_new(P->arena, N_COLREF);
            if (!n) return NULL;
            n->colref.col_idx = idx;
            return n;
        }
        case T_LPAREN:
            return p_paren_or_cast(P);
        default:
            par_err(P, "unexpected token in expression");
            return NULL;
    }
}


/* ============================================================== *
 *  Output column staging (mirrors lua_provider's LmCol)            *
 * ============================================================== */

typedef enum { LM_T_INT64 = 1, LM_T_UTF8 = 2, LM_T_BOOL = 3, LM_T_FLOAT64 = 4 } LmType;

typedef struct {
    LmType   type;
    uint8_t *nulls;
    int64_t *i64_vals;
    double  *f64_vals;
    int32_t *u8_offsets;
    char    *u8_data;
    size_t   u8_len;
    size_t   u8_cap;
    uint8_t *b_vals;
} LmCol;

static int lm_col_init(LmCol *c, LmType type, size_t length) {
    memset(c, 0, sizeof *c);
    c->type  = type;
    c->nulls = calloc(length ? length : 1, sizeof *c->nulls);
    if (!c->nulls) return -1;
    if (type == LM_T_INT64) {
        c->i64_vals = calloc(length ? length : 1, sizeof *c->i64_vals);
        if (!c->i64_vals) { free(c->nulls); c->nulls = NULL; return -1; }
    } else if (type == LM_T_FLOAT64) {
        c->f64_vals = calloc(length ? length : 1, sizeof *c->f64_vals);
        if (!c->f64_vals) { free(c->nulls); c->nulls = NULL; return -1; }
    } else if (type == LM_T_UTF8) {
        c->u8_offsets = calloc(length + 1, sizeof *c->u8_offsets);
        if (!c->u8_offsets) { free(c->nulls); c->nulls = NULL; return -1; }
        c->u8_cap = 64;
        c->u8_data = malloc(c->u8_cap);
        if (!c->u8_data) {
            free(c->u8_offsets); free(c->nulls);
            c->u8_offsets = NULL; c->nulls = NULL; return -1;
        }
    } else { /* LM_T_BOOL */
        c->b_vals = calloc(length ? length : 1, sizeof *c->b_vals);
        if (!c->b_vals) { free(c->nulls); c->nulls = NULL; return -1; }
    }
    return 0;
}

static void lm_col_free(LmCol *c) {
    free(c->nulls);
    free(c->i64_vals);
    free(c->f64_vals);
    free(c->u8_offsets);
    free(c->u8_data);
    free(c->b_vals);
    memset(c, 0, sizeof *c);
}

static int lm_col_append_string(LmCol *c, const char *s, size_t n, size_t row_idx) {
    size_t need = c->u8_len + n;
    if (need > c->u8_cap) {
        size_t nc = c->u8_cap;
        while (nc < need) nc *= 2;
        char *nd = realloc(c->u8_data, nc);
        if (!nd) return -1;
        c->u8_data = nd; c->u8_cap = nc;
    }
    if (n) memcpy(c->u8_data + c->u8_len, s, n);
    c->u8_len += n;
    c->u8_offsets[row_idx + 1] = (int32_t)c->u8_len;
    return 0;
}

static void release_int64_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 2 && arr->buffers) {
        free((void *)arr->buffers[0]);
        free((void *)arr->buffers[1]);
    }
    free(arr->buffers);
    arr->release = NULL;
}

static void release_float64_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 2 && arr->buffers) {
        free((void *)arr->buffers[0]);
        free((void *)arr->buffers[1]);
    }
    free(arr->buffers);
    arr->release = NULL;
}

static void release_utf8_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 3 && arr->buffers) {
        free((void *)arr->buffers[0]);
        free((void *)arr->buffers[1]);
        free((void *)arr->buffers[2]);
    }
    free(arr->buffers);
    arr->release = NULL;
}

static void release_bool_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 2 && arr->buffers) {
        free((void *)arr->buffers[0]);
        free((void *)arr->buffers[1]);
    }
    free(arr->buffers);
    arr->release = NULL;
}

static int lm_col_finalize(LmCol *c, size_t length, struct ArrowArray *out) {
    int64_t  null_count = 0;
    uint8_t *vmap = NULL;
    for (size_t i = 0; i < length; ++i) if (c->nulls[i]) ++null_count;
    if (null_count > 0) {
        size_t bytes = (length + 7) / 8;
        vmap = malloc(bytes ? bytes : 1);
        if (!vmap) return -1;
        memset(vmap, 0xFF, bytes ? bytes : 1);
        for (size_t i = 0; i < length; ++i) {
            if (c->nulls[i]) vmap[i / 8] &= (uint8_t)~(1u << (i % 8));
        }
    }
    free(c->nulls); c->nulls = NULL;

    if (c->type == LM_T_INT64) {
        const void **bufs = malloc(2 * sizeof *bufs);
        if (!bufs) { free(vmap); return -1; }
        bufs[0] = vmap; bufs[1] = c->i64_vals; c->i64_vals = NULL;
        out->length = (int64_t)length; out->null_count = null_count;
        out->n_buffers = 2; out->buffers = bufs; out->release = release_int64_leaf;
        return 0;
    }
    if (c->type == LM_T_FLOAT64) {
        const void **bufs = malloc(2 * sizeof *bufs);
        if (!bufs) { free(vmap); return -1; }
        bufs[0] = vmap; bufs[1] = c->f64_vals; c->f64_vals = NULL;
        out->length = (int64_t)length; out->null_count = null_count;
        out->n_buffers = 2; out->buffers = bufs; out->release = release_float64_leaf;
        return 0;
    }
    if (c->type == LM_T_UTF8) {
        /* Smooth offsets across nulls (no append was made for them). */
        int32_t last = 0;
        for (size_t i = 1; i <= length; ++i) {
            if (c->u8_offsets[i] < last) c->u8_offsets[i] = last;
            else                          last = c->u8_offsets[i];
        }
        const void **bufs = malloc(3 * sizeof *bufs);
        if (!bufs) { free(vmap); return -1; }
        bufs[0] = vmap; bufs[1] = c->u8_offsets; bufs[2] = c->u8_data;
        c->u8_offsets = NULL; c->u8_data = NULL;
        out->length = (int64_t)length; out->null_count = null_count;
        out->n_buffers = 3; out->buffers = bufs; out->release = release_utf8_leaf;
        return 0;
    }
    /* LM_T_BOOL: pack to bitmap. */
    {
        size_t bytes = (length + 7) / 8;
        uint8_t *bitmap = calloc(bytes ? bytes : 1, 1);
        if (!bitmap) { free(vmap); return -1; }
        for (size_t i = 0; i < length; ++i) {
            if (c->b_vals[i]) bitmap[i / 8] |= (uint8_t)(1u << (i % 8));
        }
        free(c->b_vals); c->b_vals = NULL;

        const void **bufs = malloc(2 * sizeof *bufs);
        if (!bufs) { free(vmap); free(bitmap); return -1; }
        bufs[0] = vmap; bufs[1] = bitmap;
        out->length = (int64_t)length; out->null_count = null_count;
        out->n_buffers = 2; out->buffers = bufs; out->release = release_bool_leaf;
        return 0;
    }
}


/* ============================================================== *
 *  Value type and evaluator                                        *
 * ============================================================== */

typedef enum { VK_INT64, VK_FLOAT64, VK_UTF8, VK_BOOL } VKind;

typedef struct {
    VKind kind;
    int   is_null;
    int64_t i64;
    double  f64;
    int     b;
    /* utf8: pointer + length. Either zero-copy into the input batch /
     * a literal node's bytes, or into the per-batch scratch arena which
     * owns the buffer until evaluate() exits. */
    const char *str;
    size_t      str_n;
} Value;

typedef struct {
    char    **owned;
    size_t    n, cap;
} ScratchStrs;

static int scratch_add(ScratchStrs *S, char *p) {
    if (!p) return 0;
    if (S->n == S->cap) {
        size_t nc = S->cap ? S->cap * 2 : 16;
        char **n = realloc(S->owned, nc * sizeof *n);
        if (!n) { free(p); return -1; }
        S->owned = n; S->cap = nc;
    }
    S->owned[S->n++] = p;
    return 0;
}

static void scratch_free(ScratchStrs *S) {
    for (size_t i = 0; i < S->n; ++i) free(S->owned[i]);
    free(S->owned);
    S->owned = NULL; S->n = S->cap = 0;
}

typedef struct {
    const struct ArrowArray *batch;
    size_t                   row;
    ScratchStrs             *scratch;
    char                     err[256];
} Eval;

static int eval_err(Eval *E, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(E->err, sizeof E->err, fmt, ap);
    va_end(ap);
    return -1;
}

/* Read row.col_idx as a Value. */
static int read_col_cell(Eval *E, size_t col_idx, char col_fmt, Value *out) {
    memset(out, 0, sizeof *out);
    const struct ArrowArray *col = E->batch->children[col_idx];
    size_t row = E->row + (size_t)col->offset;
    if (col->null_count > 0 && col->buffers[0]) {
        const uint8_t *valid = col->buffers[0];
        if (!(valid[row / 8] & (1u << (row % 8)))) { out->is_null = 1; return 0; }
    }
    switch (col_fmt) {
        case 'l': {
            const int64_t *v = col->buffers[1];
            out->kind = VK_INT64; out->i64 = v[row]; return 0;
        }
        case 'g': {
            const double *v = col->buffers[1];
            out->kind = VK_FLOAT64; out->f64 = v[row]; return 0;
        }
        case 'b': {
            const uint8_t *bitmap = col->buffers[1];
            out->kind = VK_BOOL;
            out->b = (bitmap[row / 8] >> (row % 8)) & 1u;
            return 0;
        }
        case 'u': {
            const int32_t *off = col->buffers[1];
            const char    *dat = col->buffers[2];
            out->kind  = VK_UTF8;
            out->str   = dat + off[row];
            out->str_n = (size_t)(off[row + 1] - off[row]);
            return 0;
        }
        default:
            return eval_err(E, "unsupported column format '%c'", col_fmt);
    }
}

/* Numeric promotion. If either side is float, both promote to float.
 * is_null short-circuits to NULL preserved by caller. */
static void promote_numeric(Value *a, Value *b) {
    if (a->kind == VK_FLOAT64 || b->kind == VK_FLOAT64) {
        if (a->kind == VK_INT64) { a->f64 = (double)a->i64; a->kind = VK_FLOAT64; }
        if (b->kind == VK_INT64) { b->f64 = (double)b->i64; b->kind = VK_FLOAT64; }
    }
}

static int eval_node(Eval *E, Par *P, const Node *n, Value *out);

static int op_arith(Eval *E, Par *P, Op op, const Node *na, const Node *nb, Value *out) {
    Value a, b;
    if (eval_node(E, P, na, &a) != 0) return -1;
    if (eval_node(E, P, nb, &b) != 0) return -1;
    memset(out, 0, sizeof *out);
    if (a.is_null || b.is_null) { out->is_null = 1; return 0; }

    /* String concat via `+`. */
    if (op == OP_ADD && (a.kind == VK_UTF8 || b.kind == VK_UTF8)) {
        if (a.kind != VK_UTF8 || b.kind != VK_UTF8) {
            return eval_err(E, "string + non-string not supported (cast explicitly)");
        }
        size_t need = a.str_n + b.str_n;
        char *buf = malloc(need + 1);
        if (!buf) return eval_err(E, "out of memory");
        memcpy(buf, a.str, a.str_n);
        memcpy(buf + a.str_n, b.str, b.str_n);
        buf[need] = '\0';
        if (scratch_add(E->scratch, buf) != 0) return eval_err(E, "out of memory");
        out->kind = VK_UTF8; out->str = buf; out->str_n = need;
        return 0;
    }
    if (a.kind == VK_UTF8 || b.kind == VK_UTF8 || a.kind == VK_BOOL || b.kind == VK_BOOL) {
        return eval_err(E, "arithmetic on string/bool is not allowed");
    }
    promote_numeric(&a, &b);
    if (a.kind == VK_INT64) {
        switch (op) {
            case OP_ADD: out->kind = VK_INT64; out->i64 = a.i64 + b.i64; return 0;
            case OP_SUB: out->kind = VK_INT64; out->i64 = a.i64 - b.i64; return 0;
            case OP_MUL: out->kind = VK_INT64; out->i64 = a.i64 * b.i64; return 0;
            case OP_DIV:
                if (b.i64 == 0) return eval_err(E, "integer division by zero");
                out->kind = VK_INT64; out->i64 = a.i64 / b.i64; return 0;
            case OP_MOD:
                if (b.i64 == 0) return eval_err(E, "integer modulo by zero");
                out->kind = VK_INT64; out->i64 = a.i64 % b.i64; return 0;
            default: return eval_err(E, "internal: bad arith op");
        }
    }
    /* float */
    switch (op) {
        case OP_ADD: out->kind = VK_FLOAT64; out->f64 = a.f64 + b.f64; return 0;
        case OP_SUB: out->kind = VK_FLOAT64; out->f64 = a.f64 - b.f64; return 0;
        case OP_MUL: out->kind = VK_FLOAT64; out->f64 = a.f64 * b.f64; return 0;
        case OP_DIV: out->kind = VK_FLOAT64; out->f64 = a.f64 / b.f64; return 0;
        case OP_MOD: out->kind = VK_FLOAT64; out->f64 = fmod(a.f64, b.f64); return 0;
        default: return eval_err(E, "internal: bad arith op");
    }
}

static int cmp_values(Eval *E, const Value *a, const Value *b, int *out_cmp) {
    if (a->kind == VK_UTF8 && b->kind == VK_UTF8) {
        size_t m = a->str_n < b->str_n ? a->str_n : b->str_n;
        int c = m ? memcmp(a->str, b->str, m) : 0;
        if (c != 0) { *out_cmp = c < 0 ? -1 : 1; return 0; }
        if (a->str_n == b->str_n) { *out_cmp = 0; return 0; }
        *out_cmp = a->str_n < b->str_n ? -1 : 1;
        return 0;
    }
    if (a->kind == VK_BOOL && b->kind == VK_BOOL) {
        *out_cmp = (a->b > b->b) - (a->b < b->b);
        return 0;
    }
    /* numeric */
    Value aa = *a, bb = *b;
    if (aa.kind == VK_UTF8 || bb.kind == VK_UTF8 || aa.kind == VK_BOOL || bb.kind == VK_BOOL) {
        return eval_err(E, "cannot compare mixed types");
    }
    promote_numeric(&aa, &bb);
    if (aa.kind == VK_INT64) {
        *out_cmp = (aa.i64 > bb.i64) - (aa.i64 < bb.i64);
    } else {
        *out_cmp = (aa.f64 > bb.f64) - (aa.f64 < bb.f64);
    }
    return 0;
}

static int op_cmp(Eval *E, Par *P, Op op, const Node *na, const Node *nb, Value *out) {
    Value a, b;
    if (eval_node(E, P, na, &a) != 0) return -1;
    if (eval_node(E, P, nb, &b) != 0) return -1;
    memset(out, 0, sizeof *out);
    if (a.is_null || b.is_null) { out->is_null = 1; return 0; }
    int c = 0;
    if (cmp_values(E, &a, &b, &c) != 0) return -1;
    out->kind = VK_BOOL;
    switch (op) {
        case OP_EQ: out->b = (c == 0); break;
        case OP_NE: out->b = (c != 0); break;
        case OP_LT: out->b = (c <  0); break;
        case OP_LE: out->b = (c <= 0); break;
        case OP_GT: out->b = (c >  0); break;
        case OP_GE: out->b = (c >= 0); break;
        default:    return eval_err(E, "internal: bad cmp op");
    }
    return 0;
}

/* Three-valued logical AND/OR.
 *
 *   AND: F && _ = F                   short-circuits
 *        T && T = T,  T && F = F,  T && N = N
 *        N && T = N,  N && F = F,  N && N = N
 *
 *   OR:  T || _ = T                   short-circuits
 *        F || T = T,  F || F = F,  F || N = N
 *        N || T = T,  N || F = N,  N || N = N
 */
static int op_and_or(Eval *E, Par *P, Op op, const Node *na, const Node *nb, Value *out) {
    Value a;
    if (eval_node(E, P, na, &a) != 0) return -1;
    memset(out, 0, sizeof *out);
    if (!a.is_null && a.kind != VK_BOOL) return eval_err(E, "logical operand must be boolean");

    /* Short-circuit on definite outcomes. */
    if (op == OP_AND && !a.is_null && a.b == 0) { out->kind = VK_BOOL; out->b = 0; return 0; }
    if (op == OP_OR  && !a.is_null && a.b == 1) { out->kind = VK_BOOL; out->b = 1; return 0; }

    Value b;
    if (eval_node(E, P, nb, &b) != 0) return -1;
    if (!b.is_null && b.kind != VK_BOOL) return eval_err(E, "logical operand must be boolean");

    if (op == OP_AND) {
        /* a is T or N here (the F case was short-circuited above). */
        if (b.is_null) { out->is_null = 1; return 0; }       /* T&N / N&N */
        if (b.b == 0)  { out->kind = VK_BOOL; out->b = 0; return 0; }  /* T&F / N&F */
        /* b is T. Result mirrors a: T&T=T, N&T=N. */
        if (a.is_null) { out->is_null = 1; return 0; }
        out->kind = VK_BOOL; out->b = 1; return 0;
    } else { /* OR */
        /* a is F or N here (the T case was short-circuited above). */
        if (b.is_null) { out->is_null = 1; return 0; }       /* F|N / N|N */
        if (b.b == 1)  { out->kind = VK_BOOL; out->b = 1; return 0; }  /* F|T / N|T */
        /* b is F. Result mirrors a: F|F=F, N|F=N. */
        if (a.is_null) { out->is_null = 1; return 0; }
        out->kind = VK_BOOL; out->b = 0; return 0;
    }
}

static int do_cast(Eval *E, SsisDt dt, int has_len, int64_t len, const Value *in, Value *out) {
    (void)has_len; (void)len;
    memset(out, 0, sizeof *out);
    if (in->is_null) { out->is_null = 1; return 0; }
    int to_int = (dt == DT_I1 || dt == DT_I2 || dt == DT_I4 || dt == DT_I8);
    int to_float = (dt == DT_R4 || dt == DT_R8);
    int to_str = (dt == DT_WSTR || dt == DT_STR);
    int to_bool = (dt == DT_BOOL);

    if (to_int) {
        out->kind = VK_INT64;
        if (in->kind == VK_INT64)   out->i64 = in->i64;
        else if (in->kind == VK_FLOAT64) out->i64 = (int64_t)in->f64;
        else if (in->kind == VK_BOOL)    out->i64 = in->b ? 1 : 0;
        else if (in->kind == VK_UTF8) {
            char tmp[64];
            size_t n = in->str_n < sizeof tmp - 1 ? in->str_n : sizeof tmp - 1;
            memcpy(tmp, in->str, n); tmp[n] = '\0';
            char *end = NULL; errno = 0;
            long long v = strtoll(tmp, &end, 10);
            if (end == tmp || *end != '\0' || errno != 0) return eval_err(E, "cast string -> int failed: '%s'", tmp);
            out->i64 = (int64_t)v;
        }
        return 0;
    }
    if (to_float) {
        out->kind = VK_FLOAT64;
        if (in->kind == VK_INT64)        out->f64 = (double)in->i64;
        else if (in->kind == VK_FLOAT64) out->f64 = in->f64;
        else if (in->kind == VK_BOOL)    out->f64 = in->b ? 1.0 : 0.0;
        else if (in->kind == VK_UTF8) {
            char tmp[64];
            size_t n = in->str_n < sizeof tmp - 1 ? in->str_n : sizeof tmp - 1;
            memcpy(tmp, in->str, n); tmp[n] = '\0';
            char *end = NULL; errno = 0;
            double v = strtod(tmp, &end);
            if (end == tmp || *end != '\0' || errno != 0) return eval_err(E, "cast string -> float failed: '%s'", tmp);
            out->f64 = v;
        }
        return 0;
    }
    if (to_bool) {
        out->kind = VK_BOOL;
        if (in->kind == VK_BOOL)    out->b = in->b;
        else if (in->kind == VK_INT64) out->b = in->i64 != 0;
        else if (in->kind == VK_FLOAT64) out->b = in->f64 != 0.0;
        else return eval_err(E, "cast string -> bool not supported");
        return 0;
    }
    if (to_str) {
        out->kind = VK_UTF8;
        if (in->kind == VK_UTF8) {
            out->str = in->str; out->str_n = in->str_n; return 0;
        }
        char buf[64]; int n = 0;
        if (in->kind == VK_INT64)        n = snprintf(buf, sizeof buf, "%" PRId64, in->i64);
        else if (in->kind == VK_FLOAT64) n = snprintf(buf, sizeof buf, "%g", in->f64);
        else if (in->kind == VK_BOOL)    n = snprintf(buf, sizeof buf, "%s", in->b ? "True" : "False");
        if (n < 0 || (size_t)n >= sizeof buf) return eval_err(E, "cast -> string overflow");
        char *cp = malloc((size_t)n + 1);
        if (!cp) return eval_err(E, "out of memory");
        memcpy(cp, buf, (size_t)n + 1);
        if (scratch_add(E->scratch, cp) != 0) return eval_err(E, "out of memory");
        out->str = cp; out->str_n = (size_t)n;
        return 0;
    }
    return eval_err(E, "unsupported cast target");
}

static int eval_node(Eval *E, Par *P, const Node *n, Value *out) {
    memset(out, 0, sizeof *out);
    switch (n->kind) {
        case N_LIT_INT:   out->kind = VK_INT64;   out->i64 = n->i64; return 0;
        case N_LIT_FLOAT: out->kind = VK_FLOAT64; out->f64 = n->f64; return 0;
        case N_LIT_STR:
            out->kind = VK_UTF8;
            out->str   = n->str.s;     /* stable across the engine's lifetime */
            out->str_n = n->str.n;
            return 0;
        case N_LIT_BOOL: out->kind = VK_BOOL; out->b = n->b; return 0;
        case N_LIT_NULL: out->is_null = 1; return 0;
        case N_COLREF:
            return read_col_cell(E, n->colref.col_idx,
                                 P->col_fmts[n->colref.col_idx], out);
        case N_UNARY: {
            Value a;
            if (eval_node(E, P, n->unary.a, &a) != 0) return -1;
            if (a.is_null) { out->is_null = 1; return 0; }
            switch (n->unary.op) {
                case OP_NEG:
                    if (a.kind == VK_INT64)   { out->kind = VK_INT64;   out->i64 = -a.i64; return 0; }
                    if (a.kind == VK_FLOAT64) { out->kind = VK_FLOAT64; out->f64 = -a.f64; return 0; }
                    return eval_err(E, "unary - on non-numeric");
                case OP_NOT:
                    if (a.kind != VK_BOOL) return eval_err(E, "unary ! on non-boolean");
                    out->kind = VK_BOOL; out->b = !a.b; return 0;
                case OP_BNOT:
                    if (a.kind != VK_INT64) return eval_err(E, "unary ~ on non-integer");
                    out->kind = VK_INT64; out->i64 = ~a.i64; return 0;
                default: return eval_err(E, "internal: bad unary op");
            }
        }
        case N_BINOP: {
            Op op = n->binop.op;
            if (op == OP_AND || op == OP_OR) return op_and_or(E, P, op, n->binop.a, n->binop.b, out);
            if (op == OP_EQ || op == OP_NE || op == OP_LT || op == OP_LE
                || op == OP_GT || op == OP_GE)
                return op_cmp(E, P, op, n->binop.a, n->binop.b, out);
            return op_arith(E, P, op, n->binop.a, n->binop.b, out);
        }
        case N_TERNARY: {
            Value c;
            if (eval_node(E, P, n->ternary.cond, &c) != 0) return -1;
            if (c.is_null) { out->is_null = 1; return 0; }
            if (c.kind != VK_BOOL) return eval_err(E, "ternary condition must be boolean");
            return eval_node(E, P, c.b ? n->ternary.t : n->ternary.f, out);
        }
        case N_CAST: {
            Value a;
            if (eval_node(E, P, n->cast.a, &a) != 0) return -1;
            return do_cast(E, n->cast.dt, n->cast.has_len, n->cast.len, &a, out);
        }
    }
    return eval_err(E, "internal: unknown node");
}


/* ============================================================== *
 *  Engine ABI                                                      *
 * ============================================================== */

typedef struct {
    BetlContext *ctx;
    Arena        arena;
    Node        *root;
    /* Schema cache. */
    char       **col_names;
    char        *col_fmts;
    size_t       n_cols;
    char         last_err[512];
} SsisExpr;

static int cache_schema(SsisExpr *e, const struct ArrowSchema *sch) {
    if (!sch || !sch->format || strcmp(sch->format, "+s") != 0 || sch->n_children <= 0) {
        snprintf(e->last_err, sizeof e->last_err,
                 "ssisexpr: input schema must be a struct with >=1 child");
        return -1;
    }
    size_t n = (size_t)sch->n_children;
    e->col_names = calloc(n, sizeof *e->col_names);
    e->col_fmts  = calloc(n, sizeof *e->col_fmts);
    if (!e->col_names || !e->col_fmts) {
        snprintf(e->last_err, sizeof e->last_err, "ssisexpr: out of memory");
        return -1;
    }
    for (size_t i = 0; i < n; ++i) {
        struct ArrowSchema *c = sch->children[i];
        const char *fmt = (c && c->format) ? c->format : NULL;
        if (!fmt) {
            snprintf(e->last_err, sizeof e->last_err,
                     "ssisexpr: column %zu has no format", i);
            return -1;
        }
        if (strcmp(fmt, "l") == 0)      e->col_fmts[i] = 'l';
        else if (strcmp(fmt, "g") == 0) e->col_fmts[i] = 'g';
        else if (strcmp(fmt, "b") == 0) e->col_fmts[i] = 'b';
        else if (strcmp(fmt, "u") == 0) e->col_fmts[i] = 'u';
        else {
            snprintf(e->last_err, sizeof e->last_err,
                     "ssisexpr: column '%s' has unsupported format '%s'",
                     (c && c->name) ? c->name : "?", fmt);
            return -1;
        }
        e->col_names[i] = strdup(c->name ? c->name : "");
        if (!e->col_names[i]) {
            snprintf(e->last_err, sizeof e->last_err, "ssisexpr: out of memory");
            return -1;
        }
    }
    e->n_cols = n;
    return 0;
}

static int ssisexpr_compile(BetlContext *ctx,
                            const char *source,
                            const struct ArrowSchema *input_schema,
                            void **out_handle) {
    if (!source) {
        betl_set_error(ctx, "ssisexpr: source is NULL");
        return BETL_ERR_INVALID;
    }
    SsisExpr *e = calloc(1, sizeof *e);
    if (!e) return BETL_ERR_INTERNAL;
    e->ctx = ctx;

    if (cache_schema(e, input_schema) != 0) {
        betl_set_error(ctx, "%s", e->last_err);
        for (size_t i = 0; i < e->n_cols; ++i) free(e->col_names[i]);
        free(e->col_names); free(e->col_fmts);
        free(e);
        return BETL_ERR_TYPE;
    }

    Lex L = { .p = source };
    if (lex_all(&L) != 0) {
        betl_set_error(ctx, "ssisexpr: lex error: %s", L.err);
        lex_free(&L);
        for (size_t i = 0; i < e->n_cols; ++i) free(e->col_names[i]);
        free(e->col_names); free(e->col_fmts); free(e);
        return BETL_ERR_INVALID;
    }

    Par P = {
        .toks = L.toks, .pos = 0, .arena = &e->arena,
        .col_names = e->col_names, .col_fmts = e->col_fmts, .n_cols = e->n_cols,
    };
    Node *root = p_ternary(&P);
    if (!root) {
        betl_set_error(ctx, "ssisexpr: parse error: %s",
                       P.err[0] ? P.err : "(unspecified)");
        arena_free(&e->arena);
        lex_free(&L);
        for (size_t i = 0; i < e->n_cols; ++i) free(e->col_names[i]);
        free(e->col_names); free(e->col_fmts); free(e);
        return BETL_ERR_INVALID;
    }
    if (P.toks[P.pos].kind != T_EOF) {
        betl_set_error(ctx, "ssisexpr: trailing input after expression");
        arena_free(&e->arena);
        lex_free(&L);
        for (size_t i = 0; i < e->n_cols; ++i) free(e->col_names[i]);
        free(e->col_names); free(e->col_fmts); free(e);
        return BETL_ERR_INVALID;
    }
    e->root = root;
    lex_free(&L);
    *out_handle = e;
    return BETL_OK;
}

static void ssisexpr_release(void *handle) {
    if (!handle) return;
    SsisExpr *e = handle;
    arena_free(&e->arena);
    for (size_t i = 0; i < e->n_cols; ++i) free(e->col_names[i]);
    free(e->col_names);
    free(e->col_fmts);
    free(e);
}

static int format_to_lmtype(const char *fmt, LmType *out) {
    if (!fmt) return -1;
    if      (strcmp(fmt, "l") == 0) { *out = LM_T_INT64;   return 0; }
    else if (strcmp(fmt, "g") == 0) { *out = LM_T_FLOAT64; return 0; }
    else if (strcmp(fmt, "u") == 0) { *out = LM_T_UTF8;    return 0; }
    else if (strcmp(fmt, "b") == 0) { *out = LM_T_BOOL;    return 0; }
    return -1;
}

/* Coerce one Value into the LmCol's slot for row_idx. is_null already handled. */
static int store_value(BetlContext *ctx, LmCol *col, size_t row_idx, const Value *v) {
    switch (col->type) {
        case LM_T_INT64:
            if (v->kind == VK_INT64)        col->i64_vals[row_idx] = v->i64;
            else if (v->kind == VK_FLOAT64) col->i64_vals[row_idx] = (int64_t)v->f64;
            else if (v->kind == VK_BOOL)    col->i64_vals[row_idx] = v->b ? 1 : 0;
            else { betl_set_error(ctx, "ssisexpr: cannot coerce string to int64"); return -1; }
            return 0;
        case LM_T_FLOAT64:
            if (v->kind == VK_INT64)        col->f64_vals[row_idx] = (double)v->i64;
            else if (v->kind == VK_FLOAT64) col->f64_vals[row_idx] = v->f64;
            else if (v->kind == VK_BOOL)    col->f64_vals[row_idx] = v->b ? 1.0 : 0.0;
            else { betl_set_error(ctx, "ssisexpr: cannot coerce string to float64"); return -1; }
            return 0;
        case LM_T_BOOL:
            if (v->kind == VK_BOOL)         col->b_vals[row_idx] = v->b ? 1 : 0;
            else if (v->kind == VK_INT64)   col->b_vals[row_idx] = v->i64 != 0;
            else if (v->kind == VK_FLOAT64) col->b_vals[row_idx] = v->f64 != 0.0;
            else { betl_set_error(ctx, "ssisexpr: cannot coerce string to bool"); return -1; }
            return 0;
        case LM_T_UTF8:
            if (v->kind == VK_UTF8) {
                if (lm_col_append_string(col, v->str, v->str_n, row_idx) != 0) {
                    betl_set_error(ctx, "ssisexpr: out of memory"); return -1;
                }
            } else {
                /* Stringify a non-string. Reuse the cast path semantics. */
                char buf[64]; int n = 0;
                if (v->kind == VK_INT64)        n = snprintf(buf, sizeof buf, "%" PRId64, v->i64);
                else if (v->kind == VK_FLOAT64) n = snprintf(buf, sizeof buf, "%g", v->f64);
                else /* BOOL */                 n = snprintf(buf, sizeof buf, "%s", v->b ? "True" : "False");
                if (n < 0) { betl_set_error(ctx, "ssisexpr: stringify error"); return -1; }
                if (lm_col_append_string(col, buf, (size_t)n, row_idx) != 0) {
                    betl_set_error(ctx, "ssisexpr: out of memory"); return -1;
                }
            }
            return 0;
    }
    return -1;
}

static int ssisexpr_evaluate(void *handle,
                             const struct ArrowArray *input_struct,
                             const char *desired_format,
                             struct ArrowArray *out_array) {
    SsisExpr *e = handle;
    if (!e || !input_struct || !out_array) return BETL_ERR_INVALID;
    memset(out_array, 0, sizeof *out_array);

    LmType out_type;
    if (format_to_lmtype(desired_format, &out_type) != 0) {
        snprintf(e->last_err, sizeof e->last_err,
                 "ssisexpr: unsupported desired_format '%s' (v0.1: l, g, u, b)",
                 desired_format ? desired_format : "(null)");
        betl_set_error(e->ctx, "%s", e->last_err);
        return BETL_ERR_TYPE;
    }
    if (input_struct->n_children != (int64_t)e->n_cols) {
        snprintf(e->last_err, sizeof e->last_err,
                 "ssisexpr: input batch has %lld columns, expected %zu",
                 (long long)input_struct->n_children, e->n_cols);
        betl_set_error(e->ctx, "%s", e->last_err);
        return BETL_ERR_TYPE;
    }
    size_t length = (size_t)input_struct->length;

    LmCol col;
    if (lm_col_init(&col, out_type, length) != 0) {
        betl_set_error(e->ctx, "ssisexpr: out of memory");
        return BETL_ERR_INTERNAL;
    }

    Par P = {
        .col_names = e->col_names, .col_fmts = e->col_fmts, .n_cols = e->n_cols,
    };
    ScratchStrs S = {0};
    Eval EV = { .batch = input_struct, .scratch = &S };

    for (size_t r = 0; r < length; ++r) {
        if (betl_should_cancel(e->ctx)) {
            scratch_free(&S);
            lm_col_free(&col);
            betl_set_error(e->ctx, "ssisexpr: cancelled");
            return BETL_ERR_CANCELLED;
        }
        EV.row = r;
        Value v;
        if (eval_node(&EV, &P, e->root, &v) != 0) {
            snprintf(e->last_err, sizeof e->last_err,
                     "ssisexpr: row %zu: %s", r, EV.err);
            betl_set_error(e->ctx, "%s", e->last_err);
            scratch_free(&S);
            lm_col_free(&col);
            return BETL_ERR_INTERNAL;
        }
        if (v.is_null) {
            col.nulls[r] = 1;
            if (col.type == LM_T_UTF8) col.u8_offsets[r + 1] = (int32_t)col.u8_len;
        } else {
            if (store_value(e->ctx, &col, r, &v) != 0) {
                scratch_free(&S);
                lm_col_free(&col);
                return BETL_ERR_TYPE;
            }
        }
        /* Per-row scratch reset — string concat results don't live past
         * the row. The LmCol has already copied bytes it needs. */
        scratch_free(&S);
    }

    if (lm_col_finalize(&col, length, out_array) != 0) {
        lm_col_free(&col);
        betl_set_error(e->ctx, "ssisexpr: failed to finalize output column");
        return BETL_ERR_INTERNAL;
    }
    lm_col_free(&col);
    return BETL_OK;
}


/* ============================================================== *
 *  Provider entry                                                  *
 * ============================================================== */

static const BetlExprEngine ssisexpr_engine = {
    .lang     = "ssisexpr",
    .compile  = ssisexpr_compile,
    .evaluate = ssisexpr_evaluate,
    .release  = ssisexpr_release,
};

static const BetlProvider ssisexpr_provider = {
    .abi_version = BETL_ABI_VERSION,
    .name        = "betl-ssisexpr",
    .version     = "0.1.0",
    .license     = "Apache-2.0",
    .expr_engine = &ssisexpr_engine,
};

BETL_EXPORT const BetlProvider *betl_provider_entry(void) {
    return &ssisexpr_provider;
}
