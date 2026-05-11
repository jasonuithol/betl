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
#include <time.h>              /* clock_gettime — GETDATE */

#include "betl/provider.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
typedef __int128 i128;
#pragma GCC diagnostic pop


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
    N_CALL,            /* FNAME(arg1, arg2, ...) */
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
    DT_DBDATE,          /* Arrow tdD — days since 1970-01-01 */
    DT_DBTIMESTAMP,     /* Arrow tsu: — micros since 1970-01-01 UTC, no tz */
    DT_DBTIMESTAMP2,    /* alias for DT_DBTIMESTAMP (SSIS distinguishes
                         * by precision; we treat both as us-precision) */
    DT_NUMERIC,         /* Arrow d:p,s — int128 + (precision, scale) */
    DT_GUID,            /* Arrow w:16 — 16-byte UUID */
    DT_BYTES,           /* Arrow z — variable-length bytes */
} SsisDt;

typedef enum {
    OP_NEG, OP_NOT, OP_BNOT,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE,
    OP_AND, OP_OR,
} Op;

/* Built-in functions. ISNULL / REPLACENULL are special-cased in the
 * evaluator because they must observe NULL on their first argument
 * instead of propagating it. All other functions propagate NULL when
 * any argument is NULL. */
typedef enum {
    /* string */
    FN_LEN, FN_SUBSTRING, FN_LEFT, FN_RIGHT,
    FN_TRIM, FN_LTRIM, FN_RTRIM,
    FN_LOWER, FN_UPPER,
    FN_REPLACE, FN_FINDSTRING, FN_REVERSE,
    FN_TOKEN, FN_TOKENCOUNT,
    FN_HEX, FN_CODEPOINT,
    /* null */
    FN_ISNULL, FN_REPLACENULL,
    /* numeric */
    FN_ABS, FN_POWER, FN_SQUARE, FN_SQRT,
    FN_ROUND, FN_CEILING, FN_FLOOR, FN_SIGN,
    /* date / timestamp */
    FN_GETDATE, FN_YEAR, FN_MONTH, FN_DAY,
    FN_DATEPART, FN_DATEADD, FN_DATEDIFF,
} FnId;

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
        struct { SsisDt dt; int has_len; int64_t len;
                 int has_scale; int64_t scale; Node *a; } cast;
        struct { FnId fn; Node **args; size_t n_args; } call;
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
        if (n->kind == N_LIT_STR)      free(n->str.s);
        else if (n->kind == N_CALL)    free(n->call.args);
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
 *  Built-in function table                                         *
 * ============================================================== */

typedef struct {
    const char *name;
    FnId        id;
    int         min_arity;
    int         max_arity;
} FnSpec;

static const FnSpec g_fns[] = {
    /* string */
    { "LEN",         FN_LEN,         1, 1 },
    { "SUBSTRING",   FN_SUBSTRING,   3, 3 },
    { "LEFT",        FN_LEFT,        2, 2 },
    { "RIGHT",       FN_RIGHT,       2, 2 },
    { "TRIM",        FN_TRIM,        1, 1 },
    { "LTRIM",       FN_LTRIM,       1, 1 },
    { "RTRIM",       FN_RTRIM,       1, 1 },
    { "LOWER",       FN_LOWER,       1, 1 },
    { "UPPER",       FN_UPPER,       1, 1 },
    { "REPLACE",     FN_REPLACE,     3, 3 },
    { "FINDSTRING",  FN_FINDSTRING,  3, 3 },
    { "REVERSE",     FN_REVERSE,     1, 1 },
    { "TOKEN",       FN_TOKEN,       3, 3 },
    { "TOKENCOUNT",  FN_TOKENCOUNT,  2, 2 },
    { "HEX",         FN_HEX,         1, 1 },
    { "CODEPOINT",   FN_CODEPOINT,   1, 1 },
    /* null */
    { "ISNULL",      FN_ISNULL,      1, 1 },
    { "REPLACENULL", FN_REPLACENULL, 2, 2 },
    /* numeric */
    { "ABS",         FN_ABS,         1, 1 },
    { "POWER",       FN_POWER,       2, 2 },
    { "SQUARE",      FN_SQUARE,      1, 1 },
    { "SQRT",        FN_SQRT,        1, 1 },
    { "ROUND",       FN_ROUND,       2, 2 },
    { "CEILING",     FN_CEILING,     1, 1 },
    { "FLOOR",       FN_FLOOR,       1, 1 },
    { "SIGN",        FN_SIGN,        1, 1 },
    /* date / timestamp */
    { "GETDATE",     FN_GETDATE,     0, 0 },
    { "YEAR",        FN_YEAR,        1, 1 },
    { "MONTH",       FN_MONTH,       1, 1 },
    { "DAY",         FN_DAY,         1, 1 },
    { "DATEPART",    FN_DATEPART,    2, 2 },
    { "DATEADD",     FN_DATEADD,     3, 3 },
    { "DATEDIFF",    FN_DATEDIFF,    3, 3 },
};
static const size_t g_n_fns = sizeof g_fns / sizeof g_fns[0];

static const FnSpec *lookup_fn(const char *name) {
    for (size_t i = 0; i < g_n_fns; ++i) {
        if (strcasecmp(name, g_fns[i].name) == 0) return &g_fns[i];
    }
    return NULL;
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
    char       *col_fmts;        /* parallel: 'l'/'g'/'u'/'b'/'D'/'T'/'N' */
    int        *col_scales;      /* decimal scale per column; 0 elsewhere */
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
static Node *p_call_args(Par *P, const FnSpec *fs);

/* Recognize a SSIS DT_xxx type name. Returns 1 + writes *out on hit. */
static int parse_dt(const char *name, SsisDt *out) {
    if      (strcasecmp(name, "DT_I1")           == 0) { *out = DT_I1;           return 1; }
    else if (strcasecmp(name, "DT_I2")           == 0) { *out = DT_I2;           return 1; }
    else if (strcasecmp(name, "DT_I4")           == 0) { *out = DT_I4;           return 1; }
    else if (strcasecmp(name, "DT_I8")           == 0) { *out = DT_I8;           return 1; }
    else if (strcasecmp(name, "DT_R4")           == 0) { *out = DT_R4;           return 1; }
    else if (strcasecmp(name, "DT_R8")           == 0) { *out = DT_R8;           return 1; }
    else if (strcasecmp(name, "DT_BOOL")         == 0) { *out = DT_BOOL;         return 1; }
    else if (strcasecmp(name, "DT_WSTR")         == 0) { *out = DT_WSTR;         return 1; }
    else if (strcasecmp(name, "DT_STR")          == 0) { *out = DT_STR;          return 1; }
    else if (strcasecmp(name, "DT_DBDATE")       == 0) { *out = DT_DBDATE;       return 1; }
    else if (strcasecmp(name, "DT_DBTIMESTAMP")  == 0) { *out = DT_DBTIMESTAMP;  return 1; }
    else if (strcasecmp(name, "DT_DBTIMESTAMP2") == 0) { *out = DT_DBTIMESTAMP2; return 1; }
    else if (strcasecmp(name, "DT_NUMERIC")      == 0) { *out = DT_NUMERIC;      return 1; }
    else if (strcasecmp(name, "DT_DECIMAL")      == 0) { *out = DT_NUMERIC;      return 1; }
    else if (strcasecmp(name, "DT_GUID")         == 0) { *out = DT_GUID;         return 1; }
    else if (strcasecmp(name, "DT_BYTES")        == 0) { *out = DT_BYTES;        return 1; }
    else if (strcasecmp(name, "DT_IMAGE")        == 0) { *out = DT_BYTES;        return 1; }
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
                int has_len = 0;    int64_t length = 0;
                int has_scale = 0;  int64_t scale  = 0;
                if (match(P, T_COMMA)) {
                    if (!check(P, T_INT)) {
                        par_err(P, "cast length must be an integer literal");
                        return NULL;
                    }
                    length = peek(P)->i64;
                    advance(P);
                    has_len = 1;
                    /* Optional third arg: scale for DT_NUMERIC, or
                     * code-page for DT_STR (we accept and ignore the
                     * latter). */
                    if (match(P, T_COMMA)) {
                        if (!check(P, T_INT)) {
                            par_err(P, "cast: expected integer after second comma");
                            return NULL;
                        }
                        scale = peek(P)->i64;
                        advance(P);
                        has_scale = 1;
                    }
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
                n->cast.has_scale = has_scale;
                n->cast.scale = scale;
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
        case T_IDENT: {
            advance(P);
            /* IDENT followed by '(' is a function call; otherwise a
             * column ref. Bracketed identifiers ([X]) are never function
             * calls — they always refer to columns. */
            if (check(P, T_LPAREN)) {
                const FnSpec *fs = lookup_fn(t->str);
                if (!fs) {
                    par_err(P, "unknown function '%s'", t->str);
                    return NULL;
                }
                return p_call_args(P, fs);
            }
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

/* Parse the argument list of a function call. We've already consumed
 * the IDENT and peeked at '('. The lookup has succeeded. */
static Node *p_call_args(Par *P, const FnSpec *fs) {
    if (!match(P, T_LPAREN)) { par_err(P, "internal: expected '(' after %s", fs->name); return NULL; }
    enum { MAX_ARGS = 8 };
    Node *tmp[MAX_ARGS];
    int n = 0;
    if (!check(P, T_RPAREN)) {
        for (;;) {
            if (n >= MAX_ARGS) {
                par_err(P, "%s: too many arguments (max %d)", fs->name, MAX_ARGS);
                return NULL;
            }
            Node *a = p_ternary(P);
            if (!a) return NULL;
            tmp[n++] = a;
            if (match(P, T_COMMA)) continue;
            break;
        }
    }
    if (!match(P, T_RPAREN)) { par_err(P, "%s: expected ')'", fs->name); return NULL; }
    if (n < fs->min_arity || n > fs->max_arity) {
        if (fs->min_arity == fs->max_arity) {
            par_err(P, "%s: expected %d argument(s), got %d", fs->name, fs->min_arity, n);
        } else {
            par_err(P, "%s: expected %d..%d arguments, got %d",
                    fs->name, fs->min_arity, fs->max_arity, n);
        }
        return NULL;
    }
    Node *node = arena_new(P->arena, N_CALL);
    if (!node) return NULL;
    size_t bytes = (size_t)(n > 0 ? n : 1) * sizeof *node->call.args;
    node->call.args = malloc(bytes);
    if (!node->call.args) return NULL;
    for (int i = 0; i < n; ++i) node->call.args[i] = tmp[i];
    node->call.fn = fs->id;
    node->call.n_args = (size_t)n;
    return node;
}


/* ============================================================== *
 *  Output column staging (mirrors lua_provider's LmCol)            *
 * ============================================================== */

typedef enum {
    LM_T_INT64 = 1, LM_T_UTF8 = 2, LM_T_BOOL = 3, LM_T_FLOAT64 = 4,
    LM_T_DATE32 = 5,      /* Arrow tdD — int32 days since 1970-01-01 */
    LM_T_TIMESTAMP_US = 6, /* Arrow tsu: — int64 micros since 1970-01-01 */
} LmType;

typedef struct {
    LmType   type;
    uint8_t *nulls;
    int64_t *i64_vals;      /* used by INT64 and TIMESTAMP_US */
    double  *f64_vals;
    int32_t *u8_offsets;
    char    *u8_data;
    size_t   u8_len;
    size_t   u8_cap;
    uint8_t *b_vals;
    int32_t *d32_vals;      /* used by DATE32 */
} LmCol;

static int lm_col_init(LmCol *c, LmType type, size_t length) {
    memset(c, 0, sizeof *c);
    c->type  = type;
    c->nulls = calloc(length ? length : 1, sizeof *c->nulls);
    if (!c->nulls) return -1;
    if (type == LM_T_INT64 || type == LM_T_TIMESTAMP_US) {
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
    } else if (type == LM_T_DATE32) {
        c->d32_vals = calloc(length ? length : 1, sizeof *c->d32_vals);
        if (!c->d32_vals) { free(c->nulls); c->nulls = NULL; return -1; }
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
    free(c->d32_vals);
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

static void release_date32_leaf(struct ArrowArray *arr) {
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

    if (c->type == LM_T_INT64 || c->type == LM_T_TIMESTAMP_US) {
        const void **bufs = malloc(2 * sizeof *bufs);
        if (!bufs) { free(vmap); return -1; }
        bufs[0] = vmap; bufs[1] = c->i64_vals; c->i64_vals = NULL;
        out->length = (int64_t)length; out->null_count = null_count;
        out->n_buffers = 2; out->buffers = bufs; out->release = release_int64_leaf;
        return 0;
    }
    if (c->type == LM_T_DATE32) {
        const void **bufs = malloc(2 * sizeof *bufs);
        if (!bufs) { free(vmap); return -1; }
        bufs[0] = vmap; bufs[1] = c->d32_vals; c->d32_vals = NULL;
        out->length = (int64_t)length; out->null_count = null_count;
        out->n_buffers = 2; out->buffers = bufs; out->release = release_date32_leaf;
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

typedef enum {
    VK_INT64, VK_FLOAT64, VK_UTF8, VK_BOOL,
    VK_DATE32,         /* int32 days since 1970-01-01; stored in i64 */
    VK_TIMESTAMP_US,   /* int64 micros since 1970-01-01 UTC; stored in i64 */
    VK_DECIMAL128,     /* int128 fixed-scale; stored in d128 + dec_scale */
    VK_UUID,           /* 16 raw bytes; stored in `uuid` (zero-copy into batch) */
    VK_BYTES,          /* variable-length binary; stored in `str` + `str_n` */
} VKind;

typedef struct {
    /* Ordered widest-first to minimize struct padding (clang-analyzer
     * picked the layout). decimal128 must come first since __int128 has
     * 16-byte alignment. */
    i128        d128;          /* decimal128 magnitude */
    int64_t     i64;           /* int64 + date32 + timestamp_us */
    double      f64;
    /* utf8: pointer + length. Either zero-copy into the input batch /
     * a literal node's bytes, or into the per-batch scratch arena which
     * owns the buffer until evaluate() exits. */
    const char *str;
    size_t      str_n;
    /* uuid: 16-byte pointer. Same lifetime model as `str`. */
    const uint8_t *uuid;
    VKind       kind;
    int         is_null;
    int         b;
    int         dec_scale;     /* digits after the point, for d128 */
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
static int read_col_cell(Eval *E, size_t col_idx, char col_fmt, int dec_scale,
                         Value *out) {
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
        case 'D': {
            const int32_t *v = col->buffers[1];
            out->kind = VK_DATE32; out->i64 = (int64_t)v[row]; return 0;
        }
        case 'T': {
            const int64_t *v = col->buffers[1];
            out->kind = VK_TIMESTAMP_US; out->i64 = v[row]; return 0;
        }
        case 'N': {
            const i128 *v = col->buffers[1];
            out->kind      = VK_DECIMAL128;
            out->d128      = v[row];
            out->dec_scale = dec_scale;
            return 0;
        }
        case 'U': {
            const uint8_t *v = col->buffers[1];
            out->kind = VK_UUID;
            out->uuid = &v[row * 16];     /* zero-copy into the batch */
            return 0;
        }
        case 'B': {
            /* Variable-length binary — same offsets/data layout as
             * utf8, zero-copy into the input batch. */
            const int32_t *off = col->buffers[1];
            const char    *dat = col->buffers[2];
            out->kind  = VK_BYTES;
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

/* Forward decls for decimal128 helpers (defined further down in the
 * "Date / timestamp / decimal helpers" section). cmp_values needs them
 * before that section. */
static int parse_decimal(const char *s, size_t n, int scale, i128 *out);
static int format_decimal(i128 v, int scale, char *buf, size_t cap);
static double decimal_to_double(i128 v, int scale);
static int compare_decimals(i128 a, int sa, i128 b, int sb);
static int dec_add(i128 a, int sa, i128 b, int sb, i128 *out, int *out_scale);
static int dec_sub(i128 a, int sa, i128 b, int sb, i128 *out, int *out_scale);
static int dec_mul(i128 a, int sa, i128 b, int sb, i128 *out, int *out_scale);
static int dec_div(i128 a, int sa, i128 b, int sb, i128 *out, int *out_scale);
static int dec_mod(i128 a, int sa, i128 b, int sb, i128 *out, int *out_scale);

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

    /* Decimal arithmetic. Mixed-with-float goes via doubles (lossy but
     * matches the comparison-mixing rule); mixed-with-int promotes the
     * int side to a scale-0 decimal so all-i128 paths stay exact. */
    if (a.kind == VK_DECIMAL128 || b.kind == VK_DECIMAL128) {
        if (a.kind == VK_FLOAT64 || b.kind == VK_FLOAT64) {
            double ad = (a.kind == VK_DECIMAL128) ? decimal_to_double(a.d128, a.dec_scale) : a.f64;
            double bd = (b.kind == VK_DECIMAL128) ? decimal_to_double(b.d128, b.dec_scale) : b.f64;
            out->kind = VK_FLOAT64;
            switch (op) {
                case OP_ADD: out->f64 = ad + bd; return 0;
                case OP_SUB: out->f64 = ad - bd; return 0;
                case OP_MUL: out->f64 = ad * bd; return 0;
                case OP_DIV: out->f64 = ad / bd; return 0;
                case OP_MOD: out->f64 = fmod(ad, bd); return 0;
                default:     return eval_err(E, "internal: bad arith op");
            }
        }
        if (a.kind == VK_INT64) { a.d128 = (i128)a.i64; a.dec_scale = 0; a.kind = VK_DECIMAL128; }
        if (b.kind == VK_INT64) { b.d128 = (i128)b.i64; b.dec_scale = 0; b.kind = VK_DECIMAL128; }
        i128 r; int rs; int rc;
        switch (op) {
            case OP_ADD: rc = dec_add(a.d128, a.dec_scale, b.d128, b.dec_scale, &r, &rs); break;
            case OP_SUB: rc = dec_sub(a.d128, a.dec_scale, b.d128, b.dec_scale, &r, &rs); break;
            case OP_MUL: rc = dec_mul(a.d128, a.dec_scale, b.d128, b.dec_scale, &r, &rs); break;
            case OP_DIV: rc = dec_div(a.d128, a.dec_scale, b.d128, b.dec_scale, &r, &rs); break;
            case OP_MOD: rc = dec_mod(a.d128, a.dec_scale, b.d128, b.dec_scale, &r, &rs); break;
            default:     return eval_err(E, "internal: bad arith op");
        }
        if (rc != 0) return eval_err(E, "decimal arithmetic: overflow or divide-by-zero");
        out->kind = VK_DECIMAL128; out->d128 = r; out->dec_scale = rs;
        return 0;
    }

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
    /* UUID equality / ordering: lexicographic byte compare. */
    if (a->kind == VK_UUID && b->kind == VK_UUID) {
        int c = memcmp(a->uuid, b->uuid, 16);
        *out_cmp = c < 0 ? -1 : (c > 0 ? 1 : 0);
        return 0;
    }
    /* Binary equality / ordering: lexicographic byte compare. */
    if (a->kind == VK_BYTES && b->kind == VK_BYTES) {
        size_t m = a->str_n < b->str_n ? a->str_n : b->str_n;
        int c = m ? memcmp(a->str, b->str, m) : 0;
        if (c != 0) { *out_cmp = c < 0 ? -1 : 1; return 0; }
        if (a->str_n == b->str_n) { *out_cmp = 0; return 0; }
        *out_cmp = a->str_n < b->str_n ? -1 : 1;
        return 0;
    }
    /* Decimal vs decimal — direct compare with scale promotion. */
    if (a->kind == VK_DECIMAL128 && b->kind == VK_DECIMAL128) {
        *out_cmp = compare_decimals(a->d128, a->dec_scale, b->d128, b->dec_scale);
        return 0;
    }
    /* Decimal vs other numeric: compare via double (lossy but pragmatic;
     * SSIS users mixing decimal with int/float in a comparison is rare
     * and the double has 53-bit mantissa = ~16 decimal digits headroom). */
    if (a->kind == VK_DECIMAL128 && (b->kind == VK_INT64 || b->kind == VK_FLOAT64)) {
        double ad = decimal_to_double(a->d128, a->dec_scale);
        double bd = (b->kind == VK_INT64) ? (double)b->i64 : b->f64;
        *out_cmp = (ad > bd) - (ad < bd);
        return 0;
    }
    if (b->kind == VK_DECIMAL128 && (a->kind == VK_INT64 || a->kind == VK_FLOAT64)) {
        double ad = (a->kind == VK_INT64) ? (double)a->i64 : a->f64;
        double bd = decimal_to_double(b->d128, b->dec_scale);
        *out_cmp = (ad > bd) - (ad < bd);
        return 0;
    }
    /* Temporal: same kind compares i64 directly; date vs timestamp
     * promotes date → timestamp (midnight). */
    int a_temp = (a->kind == VK_DATE32 || a->kind == VK_TIMESTAMP_US);
    int b_temp = (b->kind == VK_DATE32 || b->kind == VK_TIMESTAMP_US);
    if (a_temp && b_temp) {
        int64_t av = a->kind == VK_DATE32 ? a->i64 * 86400000000LL : a->i64;
        int64_t bv = b->kind == VK_DATE32 ? b->i64 * 86400000000LL : b->i64;
        if (a->kind == VK_DATE32 && b->kind == VK_DATE32) {
            /* both dates — compare directly to avoid the *86400e6 round-trip */
            av = a->i64; bv = b->i64;
        }
        *out_cmp = (av > bv) - (av < bv);
        return 0;
    }
    if (a_temp != b_temp) {
        return eval_err(E, "cannot compare temporal with non-temporal");
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

/* ============================================================== *
 *  Date / timestamp helpers                                        *
 *  Civil ↔ days conversions (Howard Hinnant) work for the full     *
 *  Arrow date32 range. Timestamps are micros since 1970-01-01 UTC. *
 * ============================================================== */

static int32_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)(153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int32_t)(era * 146097 + (int)doe - 719468);
}

static void civil_from_days(int32_t z, int *y, unsigned *m, unsigned *d) {
    z += 719468;
    int era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int year = (int)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = year + (*m <= 2);
}

/* Euclidean (floor) division used to split a timestamp into (days, micros). */
static void split_ts(int64_t us, int32_t *out_days, int64_t *out_us_of_day) {
    int64_t day_us = 86400000000LL;
    int64_t d = us / day_us;
    int64_t r = us % day_us;
    if (r < 0) { r += day_us; --d; }
    *out_days = (int32_t)d;
    *out_us_of_day = r;
}

/* "YYYY-MM-DD" → days. Returns -1 on parse error. */
static int parse_iso_date(const char *s, size_t n, int32_t *out_days) {
    if (n != 10) return -1;
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        int expect_digit = (i != 4 && i != 7);
        if (expect_digit && !(c >= '0' && c <= '9')) return -1;
        if (!expect_digit && c != '-') return -1;
    }
    int y = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
    unsigned mo = (unsigned)((s[5]-'0')*10 + (s[6]-'0'));
    unsigned dy = (unsigned)((s[8]-'0')*10 + (s[9]-'0'));
    if (mo < 1 || mo > 12 || dy < 1 || dy > 31) return -1;
    *out_days = days_from_civil(y, mo, dy);
    return 0;
}

/* "YYYY-MM-DD HH:MM:SS[.uuuuuu]" or with 'T' separator → micros.
 * Fractional seconds are accepted up to 6 digits, padded with zeros. */
static int parse_iso_ts(const char *s, size_t n, int64_t *out_us) {
    if (n < 19) return -1;
    int32_t days;
    if (parse_iso_date(s, 10, &days) != 0) return -1;
    if (s[10] != ' ' && s[10] != 'T') return -1;
    for (size_t i = 11; i < 19; ++i) {
        char c = s[i];
        int expect_digit = (i != 13 && i != 16);
        if (expect_digit && !(c >= '0' && c <= '9')) return -1;
        if (!expect_digit && c != ':') return -1;
    }
    int hh = (s[11]-'0')*10 + (s[12]-'0');
    int mm = (s[14]-'0')*10 + (s[15]-'0');
    int ss = (s[17]-'0')*10 + (s[18]-'0');
    if (hh > 23 || mm > 59 || ss > 59) return -1;
    int64_t us_of_day = (int64_t)hh * 3600000000LL
                      + (int64_t)mm * 60000000LL
                      + (int64_t)ss * 1000000LL;
    if (n > 19) {
        if (s[19] != '.') return -1;
        if (n > 26) return -1;            /* >6 fractional digits */
        int frac = 0; int mult = 100000;
        for (size_t i = 20; i < n; ++i) {
            char c = s[i];
            if (c < '0' || c > '9') return -1;
            frac += (c - '0') * mult;
            mult /= 10;
        }
        us_of_day += frac;
    }
    *out_us = (int64_t)days * 86400000000LL + us_of_day;
    return 0;
}

/* Format a date32 as YYYY-MM-DD. Returns bytes written (no NUL), or -1. */
static int fmt_date_iso(int32_t days, char *buf, size_t cap) {
    int y = 0; unsigned m = 0, d = 0;
    civil_from_days(days, &y, &m, &d);
    int n = snprintf(buf, cap, "%04d-%02u-%02u", y, m, d);
    if (n < 0 || (size_t)n >= cap) return -1;
    return n;
}

/* Format a ts_us as YYYY-MM-DD HH:MM:SS[.uuuuuu] — fractional part is
 * omitted when zero, matching SSIS / SQL behaviour. */
static int fmt_ts_iso(int64_t us, char *buf, size_t cap) {
    int32_t days; int64_t us_of_day;
    split_ts(us, &days, &us_of_day);
    int y = 0; unsigned m = 0, d = 0;
    civil_from_days(days, &y, &m, &d);
    int hh = (int)(us_of_day / 3600000000LL);
    int mm = (int)((us_of_day / 60000000LL) % 60);
    int ss = (int)((us_of_day / 1000000LL) % 60);
    int frac = (int)(us_of_day % 1000000LL);
    int n;
    if (frac == 0) {
        n = snprintf(buf, cap, "%04d-%02u-%02u %02d:%02d:%02d", y, m, d, hh, mm, ss);
    } else {
        n = snprintf(buf, cap, "%04d-%02u-%02u %02d:%02d:%02d.%06d",
                     y, m, d, hh, mm, ss, frac);
    }
    if (n < 0 || (size_t)n >= cap) return -1;
    return n;
}


/* ============================================================== *
 *  Decimal128 helpers (parse, format, compare)                     *
 *  Same shape as src/runtime/decimal_util — duplicated here so the *
 *  ssisexpr plugin stays self-contained.                           *
 * ============================================================== */

static int dec_safe_mul10(i128 *v, int add_digit) {
    i128 ten = 10;
    if (*v > 0 && *v > ((i128)1 << 126) / ten) return -1;
    *v *= ten;
    if (add_digit) *v += add_digit;
    return 0;
}

static int parse_decimal(const char *s, size_t n, int scale, i128 *out) {
    if (n == 0 || scale < 0 || scale > 38) return -1;
    size_t i = 0;
    int sign = 1;
    if (s[0] == '-') { sign = -1; ++i; }
    else if (s[0] == '+') ++i;
    if (i == n) return -1;
    i128 mag = 0;
    int seen = 0, frac = 0, in_frac = 0;
    for (; i < n; ++i) {
        char c = s[i];
        if (c == '.') {
            if (in_frac) return -1;
            in_frac = 1; continue;
        }
        if (c < '0' || c > '9') return -1;
        seen = 1;
        if (in_frac && frac >= scale) {
            if (c != '0') return -1;
            continue;
        }
        if (dec_safe_mul10(&mag, c - '0') != 0) return -1;
        if (in_frac) ++frac;
    }
    if (!seen) return -1;
    while (frac < scale) {
        if (dec_safe_mul10(&mag, 0) != 0) return -1;
        ++frac;
    }
    *out = (sign < 0) ? -mag : mag;
    return 0;
}

static int format_decimal(i128 v, int scale, char *buf, size_t cap) {
    if (scale < 0 || scale > 38) return -1;
    int negative = (v < 0);
    i128 mag = negative ? -v : v;
    char digits[40];
    int n = 0;
    if (mag == 0) digits[n++] = '0';
    else {
        while (mag > 0 && n < (int)sizeof digits) {
            digits[n++] = (char)('0' + (int)(mag % 10));
            mag /= 10;
        }
        if (mag > 0) return -1;
    }
    while (n < scale + 1) digits[n++] = '0';
    size_t need = (size_t)n + (scale > 0 ? 1 : 0) + (negative ? 1 : 0);
    if (need + 1 > cap) return -1;
    char *p = buf;
    if (negative) *p++ = '-';
    for (int k = n - 1; k >= scale; --k) *p++ = digits[k];
    if (scale > 0) {
        *p++ = '.';
        for (int k = scale - 1; k >= 0; --k) *p++ = digits[k];
    }
    *p = '\0';
    return (int)(p - buf);
}

/* UUID inline helpers, same shape as src/runtime/uuid_util. */
static int uuid_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int parse_uuid(const char *s, size_t n, uint8_t out[16]) {
    if (n != 36) return -1;
    if (s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-') return -1;
    int byte = 0;
    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) continue;
        int h = uuid_hex(s[i]);     if (h < 0) return -1;
        int l = uuid_hex(s[i + 1]); if (l < 0) return -1;
        out[byte++] = (uint8_t)((h << 4) | l);
        ++i;
    }
    return byte == 16 ? 0 : -1;
}
static int format_uuid(const uint8_t in[16], char *buf, size_t cap) {
    if (cap < 36) return -1;
    static const char hex[] = "0123456789abcdef";
    int byte = 0;
    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { buf[i] = '-'; continue; }
        uint8_t b = in[byte++];
        buf[i]     = hex[b >> 4];
        buf[i + 1] = hex[b & 0xF];
        ++i;
    }
    return 36;
}


static double decimal_to_double(i128 v, int scale) {
    int neg = (v < 0);
    i128 mag = neg ? -v : v;
    uint64_t lo = (uint64_t)mag;
    uint64_t hi = (uint64_t)(mag >> 64);
    double d = (double)hi * 18446744073709551616.0 + (double)lo;
    d /= pow(10.0, scale);
    return neg ? -d : d;
}

/* Compare two decimals, possibly with different scales. Returns -1/0/1.
 * Promote the lower-scale side by multiplying by 10^(diff) — overflow
 * caps the comparison toward the larger magnitude. */
static int compare_decimals(i128 a, int sa, i128 b, int sb) {
    if (sa == sb) {
        if (a < b) return -1;
        if (a > b) return 1;
        return 0;
    }
    int diff = (sa < sb) ? sb - sa : sa - sb;
    i128 mul = 1;
    int overflow = 0;
    for (int i = 0; i < diff; ++i) {
        if (mul > ((i128)1 << 126) / 10) { overflow = 1; break; }
        mul *= 10;
    }
    if (overflow) {
        /* fall back to double for the rare deep-mismatch case */
        double da = decimal_to_double(a, sa);
        double db = decimal_to_double(b, sb);
        if (da < db) return -1;
        if (da > db) return 1;
        return 0;
    }
    i128 ax = a, bx = b;
    if (sa < sb) ax *= mul; else bx *= mul;
    if (ax < bx) return -1;
    if (ax > bx) return 1;
    return 0;
}

/* Decimal arithmetic. All return 0 on success, -1 on overflow or
 * divide-by-zero. Overflow detection uses __builtin_*_overflow on
 * __int128 (gcc/clang). */

static int dec_pow10(int n, i128 *out) {
    if (n < 0 || n > 38) return -1;
    i128 r = 1;
    for (int i = 0; i < n; ++i) {
        if (__builtin_mul_overflow(r, (i128)10, &r)) return -1;
    }
    *out = r;
    return 0;
}

static int dec_scale_up(i128 v, int n, i128 *out) {
    i128 m;
    if (dec_pow10(n, &m) != 0) return -1;
    if (__builtin_mul_overflow(v, m, out)) return -1;
    return 0;
}

static int dec_add(i128 a, int sa, i128 b, int sb, i128 *out, int *out_scale) {
    int r = sa > sb ? sa : sb;
    i128 ax, bx;
    if (dec_scale_up(a, r - sa, &ax) != 0) return -1;
    if (dec_scale_up(b, r - sb, &bx) != 0) return -1;
    if (__builtin_add_overflow(ax, bx, out)) return -1;
    *out_scale = r;
    return 0;
}

static int dec_sub(i128 a, int sa, i128 b, int sb, i128 *out, int *out_scale) {
    int r = sa > sb ? sa : sb;
    i128 ax, bx;
    if (dec_scale_up(a, r - sa, &ax) != 0) return -1;
    if (dec_scale_up(b, r - sb, &bx) != 0) return -1;
    if (__builtin_sub_overflow(ax, bx, out)) return -1;
    *out_scale = r;
    return 0;
}

static int dec_mul(i128 a, int sa, i128 b, int sb, i128 *out, int *out_scale) {
    i128 r;
    if (__builtin_mul_overflow(a, b, &r)) return -1;
    int rs = sa + sb;
    /* Cap result scale at 38 by truncating trailing digits — keeps the
     * value renderable (format_decimal tops out at 38) without losing
     * any magnitude. */
    while (rs > 38) { r /= 10; --rs; }
    *out = r;
    *out_scale = rs;
    return 0;
}

static int dec_div(i128 a, int sa, i128 b, int sb, i128 *out, int *out_scale) {
    if (b == 0) return -1;
    /* Result scale: keep at least 6 fractional digits, never less than sa.
     * Mirrors SQL Server's "min 6" rule without the full precision math —
     * sufficient for round-tripping SSIS expression semantics. */
    int rs = sa > 6 ? sa : 6;
    int shift = rs + sb - sa;
    i128 num = a;
    if (shift > 0) {
        if (dec_scale_up(a, shift, &num) != 0) return -1;
    } else if (shift < 0) {
        for (int i = 0; i < -shift; ++i) num /= 10;
    }
    int neg = ((num < 0) ^ (b < 0));
    i128 anum = num < 0 ? -num : num;
    i128 ab   = b   < 0 ? -b   : b;
    i128 q   = anum / ab;
    i128 rem = anum - q * ab;
    if (rem * 2 >= ab) q += 1;  /* half-away-from-zero */
    *out = neg ? -q : q;
    *out_scale = rs;
    return 0;
}

static int dec_mod(i128 a, int sa, i128 b, int sb, i128 *out, int *out_scale) {
    if (b == 0) return -1;
    int r = sa > sb ? sa : sb;
    i128 ax, bx;
    if (dec_scale_up(a, r - sa, &ax) != 0) return -1;
    if (dec_scale_up(b, r - sb, &bx) != 0) return -1;
    *out = ax % bx;
    *out_scale = r;
    return 0;
}


static int do_cast(Eval *E, SsisDt dt, int has_len, int64_t len,
                   int has_scale, int64_t scale,
                   const Value *in, Value *out) {
    (void)has_len; (void)len;
    memset(out, 0, sizeof *out);
    if (in->is_null) { out->is_null = 1; return 0; }
    int to_int   = (dt == DT_I1 || dt == DT_I2 || dt == DT_I4 || dt == DT_I8);
    int to_float = (dt == DT_R4 || dt == DT_R8);
    int to_str   = (dt == DT_WSTR || dt == DT_STR);
    int to_bool  = (dt == DT_BOOL);
    int to_date  = (dt == DT_DBDATE);
    int to_ts    = (dt == DT_DBTIMESTAMP || dt == DT_DBTIMESTAMP2);
    int to_dec   = (dt == DT_NUMERIC);
    int to_uuid  = (dt == DT_GUID);
    int to_bytes = (dt == DT_BYTES);

    if (to_int) {
        out->kind = VK_INT64;
        if (in->kind == VK_INT64)        out->i64 = in->i64;
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
        else if (in->kind == VK_DECIMAL128) {
            /* Truncate fractional part. */
            i128 v = in->d128;
            int s = in->dec_scale;
            int neg = (v < 0);
            i128 mag = neg ? -v : v;
            for (int k = 0; k < s; ++k) mag /= 10;
            out->i64 = neg ? -(int64_t)mag : (int64_t)mag;
        }
        else return eval_err(E, "cast date/timestamp -> int not supported");
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
        else if (in->kind == VK_DECIMAL128) {
            out->f64 = decimal_to_double(in->d128, in->dec_scale);
        }
        else return eval_err(E, "cast date/timestamp -> float not supported");
        return 0;
    }
    if (to_bool) {
        out->kind = VK_BOOL;
        if (in->kind == VK_BOOL)    out->b = in->b;
        else if (in->kind == VK_INT64) out->b = in->i64 != 0;
        else if (in->kind == VK_FLOAT64) out->b = in->f64 != 0.0;
        else if (in->kind == VK_DECIMAL128) out->b = in->d128 != 0;
        else return eval_err(E, "cast non-numeric -> bool not supported");
        return 0;
    }
    if (to_str) {
        out->kind = VK_UTF8;
        if (in->kind == VK_UTF8) {
            out->str = in->str; out->str_n = in->str_n; return 0;
        }
        char buf[64]; int n = 0;
        if (in->kind == VK_INT64)             n = snprintf(buf, sizeof buf, "%" PRId64, in->i64);
        else if (in->kind == VK_FLOAT64)      n = snprintf(buf, sizeof buf, "%g", in->f64);
        else if (in->kind == VK_BOOL)        n = snprintf(buf, sizeof buf, "%s", in->b ? "True" : "False");
        else if (in->kind == VK_DATE32)       n = fmt_date_iso((int32_t)in->i64, buf, sizeof buf);
        else if (in->kind == VK_TIMESTAMP_US) n = fmt_ts_iso  (in->i64,           buf, sizeof buf);
        else if (in->kind == VK_DECIMAL128)   n = format_decimal(in->d128, in->dec_scale, buf, sizeof buf);
        else if (in->kind == VK_UUID) {
            n = format_uuid(in->uuid, buf, sizeof buf);
        }
        else if (in->kind == VK_BYTES) {
            /* Hex-encode into a fresh scratch buffer. Skip the stack
             * 64-byte path — payloads may be larger. */
            size_t need = in->str_n * 2;
            char *cp = malloc(need + 1);
            if (!cp) return eval_err(E, "out of memory");
            static const char hex[] = "0123456789abcdef";
            for (size_t i = 0; i < in->str_n; ++i) {
                uint8_t bb = (uint8_t)in->str[i];
                cp[i*2]     = hex[bb >> 4];
                cp[i*2 + 1] = hex[bb & 0xF];
            }
            cp[need] = '\0';
            if (scratch_add(E->scratch, cp) != 0) return eval_err(E, "out of memory");
            out->str = cp; out->str_n = need;
            return 0;
        }
        if (n < 0 || (size_t)n >= sizeof buf) return eval_err(E, "cast -> string overflow");
        char *cp = malloc((size_t)n + 1);
        if (!cp) return eval_err(E, "out of memory");
        memcpy(cp, buf, (size_t)n + 1);
        if (scratch_add(E->scratch, cp) != 0) return eval_err(E, "out of memory");
        out->str = cp; out->str_n = (size_t)n;
        return 0;
    }
    if (to_date) {
        out->kind = VK_DATE32;
        if (in->kind == VK_DATE32)       { out->i64 = in->i64; return 0; }
        if (in->kind == VK_TIMESTAMP_US) {
            int32_t days; int64_t us_of_day;
            split_ts(in->i64, &days, &us_of_day);
            out->i64 = days;
            return 0;
        }
        if (in->kind == VK_UTF8) {
            int32_t days;
            if (parse_iso_date(in->str, in->str_n, &days) != 0)
                return eval_err(E, "cast string -> DT_DBDATE: expected YYYY-MM-DD");
            out->i64 = days;
            return 0;
        }
        return eval_err(E, "cast non-string/non-temporal -> DT_DBDATE not supported");
    }
    if (to_ts) {
        out->kind = VK_TIMESTAMP_US;
        if (in->kind == VK_TIMESTAMP_US) { out->i64 = in->i64; return 0; }
        if (in->kind == VK_DATE32) {
            /* midnight on that date */
            out->i64 = in->i64 * 86400000000LL;
            return 0;
        }
        if (in->kind == VK_UTF8) {
            int64_t us;
            if (parse_iso_ts(in->str, in->str_n, &us) != 0)
                return eval_err(E, "cast string -> DT_DBTIMESTAMP: expected YYYY-MM-DD HH:MM:SS[.uuuuuu]");
            out->i64 = us;
            return 0;
        }
        return eval_err(E, "cast non-string/non-temporal -> DT_DBTIMESTAMP not supported");
    }
    if (to_bytes) {
        out->kind = VK_BYTES;
        if (in->kind == VK_BYTES) {
            out->str = in->str; out->str_n = in->str_n; return 0;
        }
        if (in->kind == VK_UTF8) {
            /* Hex-decode the input string into a fresh scratch buffer. */
            if ((in->str_n & 1u) != 0)
                return eval_err(E, "cast string -> DT_BYTES: expected even-length hex");
            size_t nb = in->str_n / 2;
            uint8_t *buf = malloc(nb ? nb : 1);
            if (!buf) return eval_err(E, "out of memory");
            static const signed char hv[256] = {
                ['0']= 0,['1']= 1,['2']= 2,['3']= 3,['4']= 4,
                ['5']= 5,['6']= 6,['7']= 7,['8']= 8,['9']= 9,
                ['a']=10,['b']=11,['c']=12,['d']=13,['e']=14,['f']=15,
                ['A']=10,['B']=11,['C']=12,['D']=13,['E']=14,['F']=15,
                /* zero-init for everything else means we need a guard */
            };
            for (size_t i = 0; i < nb; ++i) {
                unsigned char h = (unsigned char)in->str[i*2];
                unsigned char l = (unsigned char)in->str[i*2 + 1];
                signed char hi = hv[h], lo = hv[l];
                /* Distinguish "real 0 nibble" from "unset slot": only
                 * literal '0' chars have value 0; everything else with
                 * a 0 entry isn't a hex digit. */
                if ((hi == 0 && h != '0') || (lo == 0 && l != '0')) {
                    free(buf);
                    return eval_err(E, "cast string -> DT_BYTES: non-hex char");
                }
                buf[i] = (uint8_t)((hi << 4) | lo);
            }
            if (scratch_add(E->scratch, (char *)buf) != 0)
                return eval_err(E, "out of memory");
            out->str = (const char *)buf; out->str_n = nb;
            return 0;
        }
        return eval_err(E, "cast non-string -> DT_BYTES not supported");
    }
    if (to_uuid) {
        out->kind = VK_UUID;
        if (in->kind == VK_UUID) { out->uuid = in->uuid; return 0; }
        if (in->kind == VK_UTF8) {
            uint8_t *buf = malloc(16);
            if (!buf) return eval_err(E, "out of memory");
            if (parse_uuid(in->str, in->str_n, buf) != 0) {
                free(buf);
                return eval_err(E, "cast string -> DT_GUID: "
                                   "expected xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx");
            }
            if (scratch_add(E->scratch, (char *)buf) != 0)
                return eval_err(E, "out of memory");
            out->uuid = buf;
            return 0;
        }
        return eval_err(E, "cast non-string -> DT_GUID not supported");
    }
    if (to_dec) {
        /* `(DT_NUMERIC, p, s)`: precision is metadata only; scale is the
         * fixed-point exponent we promote the value to. v1 accepts
         * string and numeric inputs (integer / float / existing decimal
         * with rescale). */
        if (!has_scale) {
            return eval_err(E, "(DT_NUMERIC) requires (precision, scale) "
                                "e.g. (DT_NUMERIC, 12, 2)");
        }
        int target_scale = (int)scale;
        if (target_scale < 0 || target_scale > 38) {
            return eval_err(E, "(DT_NUMERIC) scale out of range [0,38]");
        }
        out->kind = VK_DECIMAL128;
        out->dec_scale = target_scale;
        if (in->kind == VK_UTF8) {
            char tmp[64];
            size_t n = in->str_n < sizeof tmp - 1 ? in->str_n : sizeof tmp - 1;
            memcpy(tmp, in->str, n); tmp[n] = '\0';
            i128 v;
            if (parse_decimal(tmp, n, target_scale, &v) != 0)
                return eval_err(E, "cast string -> DT_NUMERIC failed: '%s'", tmp);
            out->d128 = v;
            return 0;
        }
        if (in->kind == VK_INT64) {
            i128 v = (i128)in->i64;
            for (int k = 0; k < target_scale; ++k) v *= 10;
            out->d128 = v;
            return 0;
        }
        if (in->kind == VK_FLOAT64) {
            /* Lossy via text — keeps banker's-rounding etc. at libc.
             * Use 17 fractional digits which is enough for any double. */
            char tmp[64];
            int n = snprintf(tmp, sizeof tmp, "%.*f", target_scale, in->f64);
            if (n < 0 || (size_t)n >= sizeof tmp) return eval_err(E, "cast float -> DT_NUMERIC overflow");
            i128 v;
            if (parse_decimal(tmp, (size_t)n, target_scale, &v) != 0)
                return eval_err(E, "cast float -> DT_NUMERIC failed");
            out->d128 = v;
            return 0;
        }
        if (in->kind == VK_DECIMAL128) {
            /* Rescale. */
            i128 v = in->d128;
            int diff = target_scale - in->dec_scale;
            if (diff > 0) {
                for (int k = 0; k < diff; ++k) v *= 10;
            } else if (diff < 0) {
                int neg = (v < 0);
                i128 mag = neg ? -v : v;
                for (int k = 0; k < -diff; ++k) mag /= 10;
                v = neg ? -mag : mag;
            }
            out->d128 = v;
            return 0;
        }
        return eval_err(E, "cast non-numeric/non-string -> DT_NUMERIC not supported");
    }
    return eval_err(E, "unsupported cast target");
}


/* ============================================================== *
 *  Built-in function evaluators                                    *
 * ============================================================== */

static int require_str(Eval *E, const Value *v, const char *fn) {
    if (v->kind != VK_UTF8) return eval_err(E, "%s: argument must be string", fn);
    return 0;
}
static int require_int(Eval *E, const Value *v, const char *fn) {
    if (v->kind != VK_INT64) return eval_err(E, "%s: argument must be integer", fn);
    return 0;
}
static int require_num(Eval *E, const Value *v, const char *fn) {
    if (v->kind == VK_INT64 || v->kind == VK_FLOAT64) return 0;
    return eval_err(E, "%s: argument must be numeric", fn);
}

/* All string-returning functions allocate their result and hand it to
 * the per-row scratch arena, which frees it after store_value has
 * copied the bytes into the LmCol. */
static int emit_str(Eval *E, char *buf, size_t n, Value *out) {
    if (scratch_add(E->scratch, buf) != 0) return eval_err(E, "out of memory");
    out->kind = VK_UTF8; out->str = buf; out->str_n = n;
    return 0;
}

static int fn_len(Eval *E, Value *vs, Value *out) {
    if (require_str(E, &vs[0], "LEN") != 0) return -1;
    /* v0.1: byte length. UTF-8 codepoint counting is a future enhancement. */
    out->kind = VK_INT64; out->i64 = (int64_t)vs[0].str_n;
    return 0;
}

static int fn_substring(Eval *E, Value *vs, Value *out) {
    if (require_str(E, &vs[0], "SUBSTRING") != 0) return -1;
    if (require_int(E, &vs[1], "SUBSTRING") != 0) return -1;
    if (require_int(E, &vs[2], "SUBSTRING") != 0) return -1;
    int64_t start  = vs[1].i64;       /* 1-based */
    int64_t length = vs[2].i64;
    if (start  < 1) return eval_err(E, "SUBSTRING: start must be >= 1");
    if (length < 0) return eval_err(E, "SUBSTRING: length must be >= 0");
    int64_t in_n = (int64_t)vs[0].str_n;
    int64_t s = start - 1;
    if (s >= in_n) { out->kind = VK_UTF8; out->str = ""; out->str_n = 0; return 0; }
    int64_t avail = in_n - s;
    if (length > avail) length = avail;
    char *buf = malloc((size_t)length + 1);
    if (!buf) return eval_err(E, "out of memory");
    if (length > 0) memcpy(buf, vs[0].str + s, (size_t)length);
    buf[length] = '\0';
    return emit_str(E, buf, (size_t)length, out);
}

static int fn_left(Eval *E, Value *vs, Value *out) {
    if (require_str(E, &vs[0], "LEFT") != 0) return -1;
    if (require_int(E, &vs[1], "LEFT") != 0) return -1;
    int64_t n = vs[1].i64;
    if (n < 0) n = 0;
    if (n > (int64_t)vs[0].str_n) n = (int64_t)vs[0].str_n;
    char *buf = malloc((size_t)n + 1);
    if (!buf) return eval_err(E, "out of memory");
    if (n > 0) memcpy(buf, vs[0].str, (size_t)n);
    buf[n] = '\0';
    return emit_str(E, buf, (size_t)n, out);
}

static int fn_right(Eval *E, Value *vs, Value *out) {
    if (require_str(E, &vs[0], "RIGHT") != 0) return -1;
    if (require_int(E, &vs[1], "RIGHT") != 0) return -1;
    int64_t n = vs[1].i64;
    if (n < 0) n = 0;
    if (n > (int64_t)vs[0].str_n) n = (int64_t)vs[0].str_n;
    char *buf = malloc((size_t)n + 1);
    if (!buf) return eval_err(E, "out of memory");
    if (n > 0) memcpy(buf, vs[0].str + vs[0].str_n - (size_t)n, (size_t)n);
    buf[n] = '\0';
    return emit_str(E, buf, (size_t)n, out);
}

static int trim_helper(Eval *E, Value *vs, Value *out,
                       int trim_left, int trim_right, const char *name) {
    if (require_str(E, &vs[0], name) != 0) return -1;
    size_t s = 0, e = vs[0].str_n;
    if (trim_left)  while (s < e && (unsigned char)vs[0].str[s]     <= ' ') ++s;
    if (trim_right) while (e > s && (unsigned char)vs[0].str[e - 1] <= ' ') --e;
    size_t n = e - s;
    char *buf = malloc(n + 1);
    if (!buf) return eval_err(E, "out of memory");
    /* Guard on the original length: if the input is empty, vs[0].str
     * may be NULL (Arrow allows that for length-0 string arrays), and
     * memcpy(_, NULL, _) is UB even with size 0. */
    if (vs[0].str_n != 0) memcpy(buf, vs[0].str + s, n);
    buf[n] = '\0';
    return emit_str(E, buf, n, out);
}
static int fn_trim (Eval *E, Value *vs, Value *out) { return trim_helper(E, vs, out, 1, 1, "TRIM");  }
static int fn_ltrim(Eval *E, Value *vs, Value *out) { return trim_helper(E, vs, out, 1, 0, "LTRIM"); }
static int fn_rtrim(Eval *E, Value *vs, Value *out) { return trim_helper(E, vs, out, 0, 1, "RTRIM"); }

static int case_helper(Eval *E, Value *vs, Value *out, int to_upper, const char *name) {
    if (require_str(E, &vs[0], name) != 0) return -1;
    size_t n = vs[0].str_n;
    char *buf = malloc(n + 1);
    if (!buf) return eval_err(E, "out of memory");
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)vs[0].str[i];
        buf[i] = (char)(to_upper ? toupper(c) : tolower(c));
    }
    buf[n] = '\0';
    return emit_str(E, buf, n, out);
}
static int fn_lower(Eval *E, Value *vs, Value *out) { return case_helper(E, vs, out, 0, "LOWER"); }
static int fn_upper(Eval *E, Value *vs, Value *out) { return case_helper(E, vs, out, 1, "UPPER"); }

static int fn_reverse(Eval *E, Value *vs, Value *out) {
    if (require_str(E, &vs[0], "REVERSE") != 0) return -1;
    size_t n = vs[0].str_n;
    char *buf = malloc(n + 1);
    if (!buf) return eval_err(E, "out of memory");
    for (size_t i = 0; i < n; ++i) buf[i] = vs[0].str[n - 1 - i];
    buf[n] = '\0';
    return emit_str(E, buf, n, out);
}

static int fn_replace(Eval *E, Value *vs, Value *out) {
    if (require_str(E, &vs[0], "REPLACE") != 0) return -1;
    if (require_str(E, &vs[1], "REPLACE") != 0) return -1;
    if (require_str(E, &vs[2], "REPLACE") != 0) return -1;
    const char *s = vs[0].str; size_t sn  = vs[0].str_n;
    const char *f = vs[1].str; size_t fln = vs[1].str_n;
    const char *r = vs[2].str; size_t rn  = vs[2].str_n;

    /* Empty pattern: SSIS returns the source unchanged. */
    if (fln == 0) {
        char *buf = malloc(sn + 1);
        if (!buf) return eval_err(E, "out of memory");
        if (sn > 0) memcpy(buf, s, sn);
        buf[sn] = '\0';
        return emit_str(E, buf, sn, out);
    }

    size_t cap = sn + 16, n = 0;
    char *buf = malloc(cap + 1);
    if (!buf) return eval_err(E, "out of memory");
    size_t i = 0;
    while (i < sn) {
        if (i + fln <= sn && memcmp(s + i, f, fln) == 0) {
            if (n + rn > cap) {
                while (n + rn > cap) cap *= 2;
                char *nb = realloc(buf, cap + 1);
                if (!nb) { free(buf); return eval_err(E, "out of memory"); }
                buf = nb;
            }
            if (rn > 0) memcpy(buf + n, r, rn);
            n += rn;
            i += fln;
        } else {
            if (n + 1 > cap) {
                cap *= 2;
                char *nb = realloc(buf, cap + 1);
                if (!nb) { free(buf); return eval_err(E, "out of memory"); }
                buf = nb;
            }
            buf[n++] = s[i++];
        }
    }
    buf[n] = '\0';
    return emit_str(E, buf, n, out);
}

static int fn_findstring(Eval *E, Value *vs, Value *out) {
    if (require_str(E, &vs[0], "FINDSTRING") != 0) return -1;
    if (require_str(E, &vs[1], "FINDSTRING") != 0) return -1;
    if (require_int(E, &vs[2], "FINDSTRING") != 0) return -1;
    const char *s = vs[0].str; size_t sn  = vs[0].str_n;
    const char *f = vs[1].str; size_t fln = vs[1].str_n;
    int64_t occ = vs[2].i64;
    if (occ < 1) return eval_err(E, "FINDSTRING: occurrence must be >= 1");
    out->kind = VK_INT64; out->i64 = 0;
    if (fln == 0) return 0;
    int64_t found = 0;
    size_t i = 0;
    while (i + fln <= sn) {
        if (memcmp(s + i, f, fln) == 0) {
            if (++found == occ) { out->i64 = (int64_t)(i + 1); return 0; }
            i += fln;            /* non-overlapping */
        } else {
            ++i;
        }
    }
    return 0;
}

/* TOKEN(str, delimiters, n): return the N-th (1-based) substring of
 * `str` after splitting on ANY character in `delimiters`. Matches
 * SSIS-EL: consecutive delimiters do NOT produce empty tokens, and
 * leading/trailing delimiters are skipped. Returns "" if N is past
 * the end. N < 1 is an error.
 *
 * TOKENCOUNT(str, delimiters): same splitting rules, returns count. */
static int fn_token_split(const char *s, size_t sn,
                          const char *d, size_t dn,
                          int64_t want_n,
                          const char **out_start, size_t *out_len,
                          int64_t *out_count) {
    int64_t n = 0;
    size_t i = 0;
    const char *cur_start = NULL;
    size_t      cur_len   = 0;
    /* Quick membership test via a 256-bit bitmap. */
    unsigned char is_delim[32] = {0};
    if (dn == 0) {
        /* SSIS treats an empty delimiter list as "no splits" — the
         * whole string is token #1. */
        if (out_count) *out_count = sn > 0 ? 1 : 0;
        if (want_n == 1 && sn > 0) { *out_start = s; *out_len = sn; return 1; }
        return 0;
    }
    for (size_t k = 0; k < dn; ++k) {
        unsigned char c = (unsigned char)d[k];
        is_delim[c >> 3] |= (unsigned char)(1u << (c & 7));
    }
    while (i < sn) {
        while (i < sn && (is_delim[(unsigned char)s[i] >> 3] & (1u << ((unsigned char)s[i] & 7)))) ++i;
        if (i >= sn) break;
        size_t start = i;
        while (i < sn && !(is_delim[(unsigned char)s[i] >> 3] & (1u << ((unsigned char)s[i] & 7)))) ++i;
        ++n;
        if (n == want_n) { cur_start = s + start; cur_len = i - start; }
    }
    if (out_count) *out_count = n;
    if (cur_start) { *out_start = cur_start; *out_len = cur_len; return 1; }
    return 0;
}

static int fn_token(Eval *E, Value *vs, Value *out) {
    if (require_str(E, &vs[0], "TOKEN") != 0) return -1;
    if (require_str(E, &vs[1], "TOKEN") != 0) return -1;
    if (require_int(E, &vs[2], "TOKEN") != 0) return -1;
    if (vs[2].i64 < 1) return eval_err(E, "TOKEN: index must be >= 1");
    const char *p = NULL; size_t pn = 0;
    int found = fn_token_split(vs[0].str, vs[0].str_n, vs[1].str, vs[1].str_n,
                               vs[2].i64, &p, &pn, NULL);
    if (!found) {
        char *buf = malloc(1);
        if (!buf) return eval_err(E, "out of memory");
        buf[0] = '\0';
        return emit_str(E, buf, 0, out);
    }
    char *buf = malloc(pn + 1);
    if (!buf) return eval_err(E, "out of memory");
    memcpy(buf, p, pn);
    buf[pn] = '\0';
    return emit_str(E, buf, pn, out);
}

static int fn_tokencount(Eval *E, Value *vs, Value *out) {
    if (require_str(E, &vs[0], "TOKENCOUNT") != 0) return -1;
    if (require_str(E, &vs[1], "TOKENCOUNT") != 0) return -1;
    int64_t n = 0;
    const char *p = NULL; size_t pn = 0;
    (void)fn_token_split(vs[0].str, vs[0].str_n, vs[1].str, vs[1].str_n,
                         -1, &p, &pn, &n);
    out->kind = VK_INT64; out->i64 = n;
    return 0;
}

/* HEX(n): canonical SSIS-style uppercase hex of a non-negative int,
 * no leading zeros, no "0x" prefix. HEX(0) = "0". Negative inputs
 * error (SSIS rejects them too — there's no two's-complement
 * convention for variable-width DT_I8 in the spec). */
static int fn_hex(Eval *E, Value *vs, Value *out) {
    if (require_int(E, &vs[0], "HEX") != 0) return -1;
    if (vs[0].i64 < 0) return eval_err(E, "HEX: input must be non-negative");
    uint64_t v = (uint64_t)vs[0].i64;
    char tmp[17];
    int n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    else {
        while (v) {
            unsigned d = (unsigned)(v & 0xf);
            tmp[n++] = (char)(d < 10 ? '0' + d : 'A' + (d - 10));
            v >>= 4;
        }
    }
    char *buf = malloc((size_t)n + 1);
    if (!buf) return eval_err(E, "out of memory");
    for (int i = 0; i < n; ++i) buf[i] = tmp[n - 1 - i];
    buf[n] = '\0';
    return emit_str(E, buf, (size_t)n, out);
}

/* CODEPOINT(s): return the unsigned codepoint of the first character.
 * SSIS docs phrase this as "UNICODE codepoint", and we decode UTF-8
 * to match. Empty string is an error. */
static int fn_codepoint(Eval *E, Value *vs, Value *out) {
    if (require_str(E, &vs[0], "CODEPOINT") != 0) return -1;
    if (vs[0].str_n == 0) return eval_err(E, "CODEPOINT: empty string");
    const unsigned char *p = (const unsigned char *)vs[0].str;
    size_t n = vs[0].str_n;
    uint32_t cp;
    if      ((p[0] & 0x80) == 0x00) cp = p[0];
    else if ((p[0] & 0xe0) == 0xc0 && n >= 2) cp = (uint32_t)((p[0] & 0x1f) << 6) | (p[1] & 0x3f);
    else if ((p[0] & 0xf0) == 0xe0 && n >= 3) cp = (uint32_t)((p[0] & 0x0f) << 12) | (uint32_t)((p[1] & 0x3f) << 6) | (p[2] & 0x3f);
    else if ((p[0] & 0xf8) == 0xf0 && n >= 4) cp = (uint32_t)((p[0] & 0x07) << 18) | (uint32_t)((p[1] & 0x3f) << 12) | (uint32_t)((p[2] & 0x3f) << 6) | (p[3] & 0x3f);
    else return eval_err(E, "CODEPOINT: malformed UTF-8");
    out->kind = VK_INT64; out->i64 = (int64_t)cp;
    return 0;
}

/* Numeric — ABS preserves the input's numeric kind; the rest return float. */
static double as_double(const Value *v) {
    return v->kind == VK_INT64 ? (double)v->i64 : v->f64;
}

static int fn_abs(Eval *E, Value *vs, Value *out) {
    if (require_num(E, &vs[0], "ABS") != 0) return -1;
    if (vs[0].kind == VK_INT64) {
        out->kind = VK_INT64;
        out->i64 = vs[0].i64 < 0 ? -vs[0].i64 : vs[0].i64;
    } else {
        out->kind = VK_FLOAT64; out->f64 = fabs(vs[0].f64);
    }
    return 0;
}

static int fn_power(Eval *E, Value *vs, Value *out) {
    if (require_num(E, &vs[0], "POWER") != 0) return -1;
    if (require_num(E, &vs[1], "POWER") != 0) return -1;
    out->kind = VK_FLOAT64;
    out->f64 = pow(as_double(&vs[0]), as_double(&vs[1]));
    return 0;
}

static int fn_square(Eval *E, Value *vs, Value *out) {
    if (require_num(E, &vs[0], "SQUARE") != 0) return -1;
    double d = as_double(&vs[0]);
    out->kind = VK_FLOAT64; out->f64 = d * d;
    return 0;
}

static int fn_sqrt(Eval *E, Value *vs, Value *out) {
    if (require_num(E, &vs[0], "SQRT") != 0) return -1;
    out->kind = VK_FLOAT64; out->f64 = sqrt(as_double(&vs[0]));
    return 0;
}

static int fn_round(Eval *E, Value *vs, Value *out) {
    if (require_num(E, &vs[0], "ROUND") != 0) return -1;
    if (require_int(E, &vs[1], "ROUND") != 0) return -1;
    int64_t d = vs[1].i64;
    if (d < 0 || d > 15) return eval_err(E, "ROUND: decimals must be in [0,15]");
    double m = pow(10.0, (double)d);
    out->kind = VK_FLOAT64;
    out->f64 = round(as_double(&vs[0]) * m) / m;
    return 0;
}

static int fn_ceiling(Eval *E, Value *vs, Value *out) {
    if (require_num(E, &vs[0], "CEILING") != 0) return -1;
    out->kind = VK_FLOAT64; out->f64 = ceil(as_double(&vs[0]));
    return 0;
}

static int fn_floor(Eval *E, Value *vs, Value *out) {
    if (require_num(E, &vs[0], "FLOOR") != 0) return -1;
    out->kind = VK_FLOAT64; out->f64 = floor(as_double(&vs[0]));
    return 0;
}

static int fn_sign(Eval *E, Value *vs, Value *out) {
    if (require_num(E, &vs[0], "SIGN") != 0) return -1;
    out->kind = VK_INT64;
    if (vs[0].kind == VK_INT64) {
        out->i64 = (vs[0].i64 > 0) - (vs[0].i64 < 0);
    } else {
        out->i64 = (vs[0].f64 > 0.0) - (vs[0].f64 < 0.0);
    }
    return 0;
}

/* Map a SSIS DATEPART/DATEADD part name to an internal unit code.
 * Returns -1 on unknown part. */
typedef enum {
    PU_YEAR, PU_QUARTER, PU_MONTH,
    PU_DAYOFYEAR, PU_DAY,
    PU_WEEK, PU_WEEKDAY,
    PU_HOUR, PU_MINUTE, PU_SECOND,
} PartUnit;

static int parse_part(const char *s, size_t n, PartUnit *out) {
    /* SSIS / SQL Server tolerate full names and short aliases. */
    struct { const char *name; PartUnit u; } tbl[] = {
        { "year",      PU_YEAR },      { "yyyy", PU_YEAR },   { "yy", PU_YEAR },
        { "quarter",   PU_QUARTER },   { "qq",   PU_QUARTER }, { "q",  PU_QUARTER },
        { "month",     PU_MONTH },     { "mm",   PU_MONTH },   { "m",  PU_MONTH },
        { "dayofyear", PU_DAYOFYEAR }, { "dy",   PU_DAYOFYEAR }, { "y", PU_DAYOFYEAR },
        { "day",       PU_DAY },       { "dd",   PU_DAY },     { "d",  PU_DAY },
        { "week",      PU_WEEK },      { "wk",   PU_WEEK },    { "ww", PU_WEEK },
        { "weekday",   PU_WEEKDAY },   { "dw",   PU_WEEKDAY },
        { "hour",      PU_HOUR },      { "hh",   PU_HOUR },
        { "minute",    PU_MINUTE },    { "mi",   PU_MINUTE },  { "n",  PU_MINUTE },
        { "second",    PU_SECOND },    { "ss",   PU_SECOND },  { "s",  PU_SECOND },
    };
    for (size_t i = 0; i < sizeof tbl / sizeof tbl[0]; ++i) {
        size_t L = strlen(tbl[i].name);
        if (L == n && strncasecmp(s, tbl[i].name, L) == 0) {
            *out = tbl[i].u; return 0;
        }
    }
    return -1;
}

/* Get (y, m, d) for a temporal Value, treating timestamps as their date part. */
static int temporal_to_ymd(Eval *E, const Value *v, int *y, unsigned *m, unsigned *d,
                           const char *fn) {
    if (v->kind == VK_DATE32) {
        civil_from_days((int32_t)v->i64, y, m, d);
        return 0;
    }
    if (v->kind == VK_TIMESTAMP_US) {
        int32_t days; int64_t us_of_day;
        split_ts(v->i64, &days, &us_of_day);
        civil_from_days(days, y, m, d);
        return 0;
    }
    return eval_err(E, "%s: argument must be a date or timestamp", fn);
}

static int fn_getdate(Eval *E, Value *vs, Value *out) {
    (void)vs;
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return eval_err(E, "GETDATE: clock_gettime failed");
    }
    int64_t us = (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
    out->kind = VK_TIMESTAMP_US; out->i64 = us;
    return 0;
}

static int fn_year (Eval *E, Value *vs, Value *out) {
    int y = 0; unsigned m = 0, d = 0;
    if (temporal_to_ymd(E, &vs[0], &y, &m, &d, "YEAR") != 0) return -1;
    out->kind = VK_INT64; out->i64 = y;
    return 0;
}
static int fn_month(Eval *E, Value *vs, Value *out) {
    int y = 0; unsigned m = 0, d = 0;
    if (temporal_to_ymd(E, &vs[0], &y, &m, &d, "MONTH") != 0) return -1;
    out->kind = VK_INT64; out->i64 = (int64_t)m;
    return 0;
}
static int fn_day  (Eval *E, Value *vs, Value *out) {
    int y = 0; unsigned m = 0, d = 0;
    if (temporal_to_ymd(E, &vs[0], &y, &m, &d, "DAY") != 0) return -1;
    out->kind = VK_INT64; out->i64 = (int64_t)d;
    return 0;
}

static int fn_datepart(Eval *E, Value *vs, Value *out) {
    if (vs[0].kind != VK_UTF8) return eval_err(E, "DATEPART: first arg must be a part name string");
    PartUnit pu;
    if (parse_part(vs[0].str, vs[0].str_n, &pu) != 0)
        return eval_err(E, "DATEPART: unknown part");
    int y = 0; unsigned m = 0, d = 0;
    if (temporal_to_ymd(E, &vs[1], &y, &m, &d, "DATEPART") != 0) return -1;
    out->kind = VK_INT64;
    if (pu == PU_YEAR)        { out->i64 = y; return 0; }
    if (pu == PU_QUARTER)     { out->i64 = (int64_t)((m - 1) / 3 + 1); return 0; }
    if (pu == PU_MONTH)       { out->i64 = (int64_t)m; return 0; }
    if (pu == PU_DAY)         { out->i64 = (int64_t)d; return 0; }
    if (pu == PU_DAYOFYEAR)   {
        int32_t doy = days_from_civil(y, m, d) - days_from_civil(y, 1, 1) + 1;
        out->i64 = doy; return 0;
    }
    if (pu == PU_WEEKDAY)     {
        /* SSIS DW: 1=Sunday..7=Saturday. days_from_civil's epoch
         * 1970-01-01 was a Thursday (DW=5). */
        int32_t days = days_from_civil(y, m, d);
        int32_t dw = (days % 7 + 7) % 7;            /* 0=Thu */
        int32_t ssis = ((dw + 4) % 7) + 1;          /* shift so Sunday=1 */
        out->i64 = ssis; return 0;
    }
    if (pu == PU_WEEK)        {
        /* ISO-ish week-of-year via dayofyear (simple version: 1..53). */
        int32_t doy = days_from_civil(y, m, d) - days_from_civil(y, 1, 1) + 1;
        out->i64 = (doy + 6) / 7; return 0;
    }
    /* Time-of-day units require a timestamp. */
    if (vs[1].kind != VK_TIMESTAMP_US)
        return eval_err(E, "DATEPART: hour/minute/second require a timestamp");
    int32_t days; int64_t us_of_day;
    split_ts(vs[1].i64, &days, &us_of_day);
    if (pu == PU_HOUR)   { out->i64 = us_of_day / 3600000000LL; return 0; }
    if (pu == PU_MINUTE) { out->i64 = (us_of_day / 60000000LL) % 60; return 0; }
    if (pu == PU_SECOND) { out->i64 = (us_of_day / 1000000LL) % 60; return 0; }
    return eval_err(E, "DATEPART: internal: unhandled part unit");
}

/* DATEADD on a date with sub-day units rejects (matches "predictable typed"
 * behaviour: result type follows input). Use DATEADD on a timestamp for
 * hours/minutes/seconds. */
static int fn_dateadd(Eval *E, Value *vs, Value *out) {
    if (vs[0].kind != VK_UTF8) return eval_err(E, "DATEADD: first arg must be a part name string");
    if (vs[1].kind != VK_INT64) return eval_err(E, "DATEADD: second arg must be an integer");
    PartUnit pu;
    if (parse_part(vs[0].str, vs[0].str_n, &pu) != 0)
        return eval_err(E, "DATEADD: unknown part");
    int64_t n = vs[1].i64;
    int input_is_date = (vs[2].kind == VK_DATE32);
    int input_is_ts   = (vs[2].kind == VK_TIMESTAMP_US);
    if (!input_is_date && !input_is_ts)
        return eval_err(E, "DATEADD: third arg must be a date or timestamp");

    /* Convert to (days, us_of_day) so all math is uniform. */
    int32_t days; int64_t us_of_day = 0;
    if (input_is_date) { days = (int32_t)vs[2].i64; }
    else               { split_ts(vs[2].i64, &days, &us_of_day); }

    if (pu == PU_HOUR || pu == PU_MINUTE || pu == PU_SECOND) {
        if (input_is_date) return eval_err(E, "DATEADD: sub-day units require a timestamp input");
        int64_t delta_us =
            pu == PU_HOUR   ? n * 3600000000LL :
            pu == PU_MINUTE ? n *   60000000LL :
                              n *    1000000LL;
        int64_t new_us = (int64_t)days * 86400000000LL + us_of_day + delta_us;
        out->kind = VK_TIMESTAMP_US; out->i64 = new_us;
        return 0;
    }

    if (pu == PU_DAY || pu == PU_DAYOFYEAR) {
        days = (int32_t)((int64_t)days + n);
    } else if (pu == PU_WEEK) {
        days = (int32_t)((int64_t)days + n * 7);
    } else {
        /* year / quarter / month: do calendar math, then clamp day. */
        int y = 0; unsigned m = 0, d = 0;
        civil_from_days(days, &y, &m, &d);
        int64_t months;
        if (pu == PU_YEAR)         months = n * 12;
        else if (pu == PU_QUARTER) months = n * 3;
        else /* PU_MONTH */        months = n;
        /* y/m → 0-based total months from epoch year, then add. */
        int64_t total = (int64_t)y * 12 + (int64_t)(m - 1) + months;
        int ny = (int)(total / 12);
        unsigned nm = (unsigned)(((total % 12) + 12) % 12) + 1;
        if (total < 0 && (total % 12) != 0) --ny;
        /* clamp d to last day of (ny, nm) */
        static const unsigned mdays[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
        unsigned last = mdays[nm - 1];
        if (nm == 2) {
            int leap = (ny % 4 == 0 && (ny % 100 != 0 || ny % 400 == 0));
            if (leap) last = 29;
        }
        unsigned nd = d > last ? last : d;
        days = days_from_civil(ny, nm, nd);
    }

    if (input_is_date) {
        out->kind = VK_DATE32; out->i64 = days;
    } else {
        out->kind = VK_TIMESTAMP_US;
        out->i64 = (int64_t)days * 86400000000LL + us_of_day;
    }
    return 0;
}

static int fn_datediff(Eval *E, Value *vs, Value *out) {
    if (vs[0].kind != VK_UTF8) return eval_err(E, "DATEDIFF: first arg must be a part name string");
    PartUnit pu;
    if (parse_part(vs[0].str, vs[0].str_n, &pu) != 0)
        return eval_err(E, "DATEDIFF: unknown part");

    /* Coerce both temporal args to (days, us_of_day). */
    int32_t d1, d2; int64_t u1 = 0, u2 = 0;
    if (vs[1].kind == VK_DATE32)            d1 = (int32_t)vs[1].i64;
    else if (vs[1].kind == VK_TIMESTAMP_US) split_ts(vs[1].i64, &d1, &u1);
    else return eval_err(E, "DATEDIFF: second arg must be date or timestamp");
    if (vs[2].kind == VK_DATE32)            d2 = (int32_t)vs[2].i64;
    else if (vs[2].kind == VK_TIMESTAMP_US) split_ts(vs[2].i64, &d2, &u2);
    else return eval_err(E, "DATEDIFF: third arg must be date or timestamp");

    out->kind = VK_INT64;
    if (pu == PU_DAY || pu == PU_DAYOFYEAR) {
        out->i64 = (int64_t)d2 - (int64_t)d1;
        return 0;
    }
    if (pu == PU_WEEK) {
        out->i64 = ((int64_t)d2 - (int64_t)d1) / 7;
        return 0;
    }
    if (pu == PU_YEAR || pu == PU_QUARTER || pu == PU_MONTH) {
        int y1, y2; unsigned m1, m2, dd1, dd2;
        civil_from_days(d1, &y1, &m1, &dd1);
        civil_from_days(d2, &y2, &m2, &dd2);
        int64_t months = ((int64_t)y2 - y1) * 12 + ((int64_t)m2 - m1);
        if (pu == PU_YEAR)    out->i64 = months / 12;
        if (pu == PU_QUARTER) out->i64 = months / 3;
        if (pu == PU_MONTH)   out->i64 = months;
        return 0;
    }
    /* hour/minute/second: full elapsed time including us_of_day. */
    int64_t us1 = (int64_t)d1 * 86400000000LL + u1;
    int64_t us2 = (int64_t)d2 * 86400000000LL + u2;
    int64_t delta = us2 - us1;
    if (pu == PU_HOUR)   { out->i64 = delta / 3600000000LL; return 0; }
    if (pu == PU_MINUTE) { out->i64 = delta /   60000000LL; return 0; }
    if (pu == PU_SECOND) { out->i64 = delta /    1000000LL; return 0; }
    return eval_err(E, "DATEDIFF: unsupported part");
}

/* Dispatch. ISNULL/REPLACENULL must observe NULL on their first arg
 * rather than propagating; every other function propagates NULL when
 * any arg is NULL. */
static int eval_call(Eval *E, Par *P, const Node *n, Value *out) {
    FnId   fn   = n->call.fn;
    Node **args = n->call.args;
    size_t na   = n->call.n_args;
    memset(out, 0, sizeof *out);

    if (fn == FN_ISNULL) {
        Value a;
        if (eval_node(E, P, args[0], &a) != 0) return -1;
        out->kind = VK_BOOL; out->b = a.is_null ? 1 : 0;
        return 0;
    }
    if (fn == FN_REPLACENULL) {
        Value a;
        if (eval_node(E, P, args[0], &a) != 0) return -1;
        if (!a.is_null) { *out = a; return 0; }
        return eval_node(E, P, args[1], out);
    }

    /* All other functions: evaluate args, propagate NULL. The explicit
     * zero-init is for clang-analyzer's benefit — eval_node already zeros
     * each output, but the analyzer can't follow that across the array. */
    Value vs[8] = {0};     /* matches MAX_ARGS in p_call_args */
    for (size_t i = 0; i < na; ++i) {
        if (eval_node(E, P, args[i], &vs[i]) != 0) return -1;
    }
    for (size_t i = 0; i < na; ++i) {
        if (vs[i].is_null) { out->is_null = 1; return 0; }
    }

    switch (fn) {
        case FN_LEN:        return fn_len       (E, vs, out);
        case FN_SUBSTRING:  return fn_substring (E, vs, out);
        case FN_LEFT:       return fn_left      (E, vs, out);
        case FN_RIGHT:      return fn_right     (E, vs, out);
        case FN_TRIM:       return fn_trim      (E, vs, out);
        case FN_LTRIM:      return fn_ltrim     (E, vs, out);
        case FN_RTRIM:      return fn_rtrim     (E, vs, out);
        case FN_LOWER:      return fn_lower     (E, vs, out);
        case FN_UPPER:      return fn_upper     (E, vs, out);
        case FN_REPLACE:    return fn_replace   (E, vs, out);
        case FN_FINDSTRING: return fn_findstring(E, vs, out);
        case FN_REVERSE:    return fn_reverse   (E, vs, out);
        case FN_TOKEN:      return fn_token     (E, vs, out);
        case FN_TOKENCOUNT: return fn_tokencount(E, vs, out);
        case FN_HEX:        return fn_hex       (E, vs, out);
        case FN_CODEPOINT:  return fn_codepoint (E, vs, out);
        case FN_ABS:        return fn_abs       (E, vs, out);
        case FN_POWER:      return fn_power     (E, vs, out);
        case FN_SQUARE:     return fn_square    (E, vs, out);
        case FN_SQRT:       return fn_sqrt      (E, vs, out);
        case FN_ROUND:      return fn_round     (E, vs, out);
        case FN_CEILING:    return fn_ceiling   (E, vs, out);
        case FN_FLOOR:      return fn_floor     (E, vs, out);
        case FN_SIGN:       return fn_sign      (E, vs, out);
        case FN_GETDATE:    return fn_getdate   (E, vs, out);
        case FN_YEAR:       return fn_year      (E, vs, out);
        case FN_MONTH:      return fn_month     (E, vs, out);
        case FN_DAY:        return fn_day       (E, vs, out);
        case FN_DATEPART:   return fn_datepart  (E, vs, out);
        case FN_DATEADD:    return fn_dateadd   (E, vs, out);
        case FN_DATEDIFF:   return fn_datediff  (E, vs, out);
        case FN_ISNULL:
        case FN_REPLACENULL: /* handled above */ break;
    }
    return eval_err(E, "internal: unhandled function id");
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
                                 P->col_fmts[n->colref.col_idx],
                                 P->col_scales[n->colref.col_idx], out);
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
            return do_cast(E, n->cast.dt, n->cast.has_len, n->cast.len,
                           n->cast.has_scale, n->cast.scale, &a, out);
        }
        case N_CALL:
            return eval_call(E, P, n, out);
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
    int         *col_scales;     /* set for 'N' (decimal) columns; 0 elsewhere */
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
    e->col_names  = calloc(n, sizeof *e->col_names);
    e->col_fmts   = calloc(n, sizeof *e->col_fmts);
    e->col_scales = calloc(n, sizeof *e->col_scales);
    if (!e->col_names || !e->col_fmts || !e->col_scales) {
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
        if (strcmp(fmt, "l") == 0)        e->col_fmts[i] = 'l';
        else if (strcmp(fmt, "g") == 0)   e->col_fmts[i] = 'g';
        else if (strcmp(fmt, "b") == 0)   e->col_fmts[i] = 'b';
        else if (strcmp(fmt, "u") == 0)   e->col_fmts[i] = 'u';
        else if (strcmp(fmt, "tdD") == 0) e->col_fmts[i] = 'D';
        else if (strcmp(fmt, "tsu:") == 0) e->col_fmts[i] = 'T';
        else if (strcmp(fmt, "tsu:UTC") == 0) e->col_fmts[i] = 'T'; /* same int64 layout */
        else if (strcmp(fmt, "ttu") == 0)     e->col_fmts[i] = 'l'; /* time = int64 micros */
        else if (strcmp(fmt, "w:16") == 0)    e->col_fmts[i] = 'U';
        else if (strcmp(fmt, "z") == 0)       e->col_fmts[i] = 'B';
        else if (strncmp(fmt, "d:", 2) == 0) {
            /* "d:precision,scale" — extract scale (precision is metadata). */
            int p = 0, s = 0;
            if (sscanf(fmt + 2, "%d,%d", &p, &s) != 2 || s < 0 || s > 38) {
                snprintf(e->last_err, sizeof e->last_err,
                         "ssisexpr: column '%s' has malformed decimal format '%s'",
                         (c && c->name) ? c->name : "?", fmt);
                return -1;
            }
            e->col_fmts[i]   = 'N';
            e->col_scales[i] = s;
        }
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
        free(e->col_names); free(e->col_fmts); free(e->col_scales);
        free(e);
        return BETL_ERR_TYPE;
    }

    Lex L = { .p = source };
    if (lex_all(&L) != 0) {
        betl_set_error(ctx, "ssisexpr: lex error: %s", L.err);
        lex_free(&L);
        for (size_t i = 0; i < e->n_cols; ++i) free(e->col_names[i]);
        free(e->col_names); free(e->col_fmts); free(e->col_scales); free(e);
        return BETL_ERR_INVALID;
    }

    Par P = {
        .toks = L.toks, .pos = 0, .arena = &e->arena,
        .col_names = e->col_names, .col_fmts = e->col_fmts,
        .col_scales = e->col_scales, .n_cols = e->n_cols,
    };
    Node *root = p_ternary(&P);
    if (!root) {
        betl_set_error(ctx, "ssisexpr: parse error: %s",
                       P.err[0] ? P.err : "(unspecified)");
        arena_free(&e->arena);
        lex_free(&L);
        for (size_t i = 0; i < e->n_cols; ++i) free(e->col_names[i]);
        free(e->col_names); free(e->col_fmts); free(e->col_scales); free(e);
        return BETL_ERR_INVALID;
    }
    if (P.toks[P.pos].kind != T_EOF) {
        betl_set_error(ctx, "ssisexpr: trailing input after expression");
        arena_free(&e->arena);
        lex_free(&L);
        for (size_t i = 0; i < e->n_cols; ++i) free(e->col_names[i]);
        free(e->col_names); free(e->col_fmts); free(e->col_scales); free(e);
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
    free(e->col_scales);
    free(e);
}

static int format_to_lmtype(const char *fmt, LmType *out) {
    if (!fmt) return -1;
    if      (strcmp(fmt, "l")    == 0) { *out = LM_T_INT64;        return 0; }
    else if (strcmp(fmt, "g")    == 0) { *out = LM_T_FLOAT64;      return 0; }
    else if (strcmp(fmt, "u")    == 0) { *out = LM_T_UTF8;         return 0; }
    else if (strcmp(fmt, "b")    == 0) { *out = LM_T_BOOL;         return 0; }
    else if (strcmp(fmt, "tdD")  == 0) { *out = LM_T_DATE32;       return 0; }
    else if (strcmp(fmt, "tsu:") == 0) { *out = LM_T_TIMESTAMP_US; return 0; }
    return -1;
}

/* Forward decls for stringify helpers used by store_value's UTF8 path. */
static int fmt_date_iso(int32_t days, char *buf, size_t cap);
static int fmt_ts_iso  (int64_t us,   char *buf, size_t cap);

/* Coerce one Value into the LmCol's slot for row_idx. is_null already handled. */
static int store_value(BetlContext *ctx, LmCol *col, size_t row_idx, const Value *v) {
    switch (col->type) {
        case LM_T_INT64:
            if (v->kind == VK_INT64)        col->i64_vals[row_idx] = v->i64;
            else if (v->kind == VK_FLOAT64) col->i64_vals[row_idx] = (int64_t)v->f64;
            else if (v->kind == VK_BOOL)    col->i64_vals[row_idx] = v->b ? 1 : 0;
            else { betl_set_error(ctx, "ssisexpr: cannot coerce date/string to int64"); return -1; }
            return 0;
        case LM_T_FLOAT64:
            if (v->kind == VK_INT64)        col->f64_vals[row_idx] = (double)v->i64;
            else if (v->kind == VK_FLOAT64) col->f64_vals[row_idx] = v->f64;
            else if (v->kind == VK_BOOL)    col->f64_vals[row_idx] = v->b ? 1.0 : 0.0;
            else { betl_set_error(ctx, "ssisexpr: cannot coerce date/string to float64"); return -1; }
            return 0;
        case LM_T_BOOL:
            if (v->kind == VK_BOOL)         col->b_vals[row_idx] = v->b ? 1 : 0;
            else if (v->kind == VK_INT64)   col->b_vals[row_idx] = v->i64 != 0;
            else if (v->kind == VK_FLOAT64) col->b_vals[row_idx] = v->f64 != 0.0;
            else { betl_set_error(ctx, "ssisexpr: cannot coerce date/string to bool"); return -1; }
            return 0;
        case LM_T_DATE32:
            if (v->kind != VK_DATE32) {
                betl_set_error(ctx, "ssisexpr: cannot coerce non-date to DT_DBDATE column "
                                    "(use (DT_DBDATE) cast first)");
                return -1;
            }
            col->d32_vals[row_idx] = (int32_t)v->i64;
            return 0;
        case LM_T_TIMESTAMP_US:
            if (v->kind == VK_TIMESTAMP_US) {
                col->i64_vals[row_idx] = v->i64;
            } else if (v->kind == VK_DATE32) {
                /* Promote date32 to midnight-on-that-date in micros. */
                col->i64_vals[row_idx] = v->i64 * (int64_t)86400000000LL;
            } else {
                betl_set_error(ctx, "ssisexpr: cannot coerce non-temporal to DT_DBTIMESTAMP column");
                return -1;
            }
            return 0;
        case LM_T_UTF8:
            if (v->kind == VK_UTF8) {
                if (lm_col_append_string(col, v->str, v->str_n, row_idx) != 0) {
                    betl_set_error(ctx, "ssisexpr: out of memory"); return -1;
                }
            } else {
                /* Stringify a non-string. Reuse the cast path semantics. */
                char buf[64]; int n = 0;
                if (v->kind == VK_INT64)             n = snprintf(buf, sizeof buf, "%" PRId64, v->i64);
                else if (v->kind == VK_FLOAT64)      n = snprintf(buf, sizeof buf, "%g", v->f64);
                else if (v->kind == VK_BOOL)        n = snprintf(buf, sizeof buf, "%s", v->b ? "True" : "False");
                else if (v->kind == VK_DATE32)       n = fmt_date_iso((int32_t)v->i64, buf, sizeof buf);
                else if (v->kind == VK_TIMESTAMP_US) n = fmt_ts_iso  (v->i64,           buf, sizeof buf);
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
        .col_names = e->col_names, .col_fmts = e->col_fmts,
        .col_scales = e->col_scales, .n_cols = e->n_cols,
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
