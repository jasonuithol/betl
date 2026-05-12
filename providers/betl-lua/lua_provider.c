/*
 * betl-lua — Lua 5.4 language-host provider for betl.
 *
 * v0.1 contributes two components:
 *   - lua.task — a TASK that runs a user Lua chunk for side effects.
 *   - lua.map  — a TRANSFORM that runs a per-row script over an
 *                Arrow stream. Output schema = input schema (v0.1
 *                limitation; columns added to row are silently
 *                ignored). Supported column types: int64 ('l') and
 *                utf8 ('u'). float64 ('g') will piggyback on the
 *                int64 path once a source emits it.
 *
 * Host bridges exposed to the script:
 *   log.trace/debug/info/warn/error(msg)   -> betl_log
 *   params.<name>                          -> betl_get_param   (via __index)
 *   connection(name)                       -> betl_get_connection
 *
 * Sandboxing (resource caps, allow-listing of stdlib modules) is
 * deferred per SPEC §6.2; v0.1 opens the full Lua standard library.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "betl/provider.h"


/* ============================================================== *
 *  JSON-string decode                                              *
 *                                                                  *
 *  The host hands us the step config as a flat compact JSON blob   *
 *  produced by yaml_to_json_node in pipeline.c. The "script:" YAML  *
 *  literal block becomes a JSON string with standard escapes        *
 *  (\\n, \\t, \\", \\\\) and \\u00XX for control bytes. We have to  *
 *  decode those before handing the source to luaL_loadstring,       *
 *  because Lua doesn't speak JSON escapes.                          *
 * ============================================================== */

static const char *json_value_after(const char *json, const char *key) {
    if (!json || !key) return NULL;
    char needle[64];
    int n = snprintf(needle, sizeof needle, "\"%s\":", key);
    if (n < 0 || (size_t)n >= sizeof needle) return NULL;
    const char *p = strstr(json, needle);
    if (!p) return NULL;
    p += (size_t)n;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    return p;
}

/* Decode a JSON string at *p (pointing at the opening quote) into a
 * freshly malloc'd NUL-terminated buffer. Returns 0 on success, -1
 * on malformed input. Handles \" \\ \/ \n \t \r \b \f and \uXXXX
 * (BMP only — surrogate pairs are rejected). */
static int json_decode_string(const char *p, char **out) {
    *out = NULL;
    if (!p || *p != '"') return -1;
    ++p;
    /* Upper bound: every escape decodes to <= input length bytes
     * (\uXXXX → up to 3 UTF-8 bytes, < 6 input bytes). */
    size_t cap = strlen(p) + 1;
    char *buf = malloc(cap);
    if (!buf) return -1;
    size_t i = 0;
    while (*p && *p != '"') {
        if (*p != '\\') { buf[i++] = *p++; continue; }
        ++p;
        if (!*p) { free(buf); return -1; }
        switch (*p) {
            case '"':  buf[i++] = '"';  ++p; break;
            case '\\': buf[i++] = '\\'; ++p; break;
            case '/':  buf[i++] = '/';  ++p; break;
            case 'n':  buf[i++] = '\n'; ++p; break;
            case 't':  buf[i++] = '\t'; ++p; break;
            case 'r':  buf[i++] = '\r'; ++p; break;
            case 'b':  buf[i++] = '\b'; ++p; break;
            case 'f':  buf[i++] = '\f'; ++p; break;
            case 'u': {
                unsigned cp = 0;
                for (int k = 1; k <= 4; ++k) {
                    char c = p[k];
                    cp <<= 4;
                    if (c >= '0' && c <= '9') cp |= (unsigned)(c - '0');
                    else if (c >= 'a' && c <= 'f') cp |= (unsigned)(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') cp |= (unsigned)(c - 'A' + 10);
                    else { free(buf); return -1; }
                }
                /* Reject surrogate halves — pipeline.c can't emit them
                 * and we don't decode pairs in v0.1. */
                if (cp >= 0xD800 && cp <= 0xDFFF) { free(buf); return -1; }
                p += 5;
                if (cp < 0x80) {
                    buf[i++] = (char)cp;
                } else if (cp < 0x800) {
                    buf[i++] = (char)(0xC0 | (cp >> 6));
                    buf[i++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    buf[i++] = (char)(0xE0 | (cp >> 12));
                    buf[i++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    buf[i++] = (char)(0x80 | (cp & 0x3F));
                }
                break;
            }
            default: free(buf); return -1;
        }
    }
    if (*p != '"') { free(buf); return -1; }
    buf[i] = '\0';
    *out = buf;
    return 0;
}

static int json_get_string(const char *json, const char *key, char **out) {
    return json_decode_string(json_value_after(json, key), out);
}


/* ============================================================== *
 *  Host bridges                                                    *
 *                                                                  *
 *  Each C closure carries the BetlContext as upvalue 1 (light       *
 *  userdata), so the lua_State stays free of registry-key magic     *
 *  and the bindings remain reentrant.                               *
 * ============================================================== */

/* upvalue 1: BetlContext*, upvalue 2: BetlLogLevel as integer */
static int l_log_at(lua_State *L) {
    BetlContext *ctx = lua_touserdata(L, lua_upvalueindex(1));
    int level        = (int)lua_tointeger(L, lua_upvalueindex(2));
    const char *msg  = luaL_checkstring(L, 1);
    betl_log(ctx, (BetlLogLevel)level, "%s", msg);
    return 0;
}

/* params is a table with a metatable whose __index forwards to
 * betl_get_param. Reads only — assignments raise. */
static int l_params_index(lua_State *L) {
    BetlContext *ctx = lua_touserdata(L, lua_upvalueindex(1));
    const char *key  = lua_tostring(L, 2);
    if (!key) { lua_pushnil(L); return 1; }
    const char *val = betl_get_param(ctx, key);
    if (val) lua_pushstring(L, val); else lua_pushnil(L);
    return 1;
}

static int l_params_newindex(lua_State *L) {
    return luaL_error(L, "params is read-only");
}

/* connection("name") -> JSON string or nil */
static int l_connection(lua_State *L) {
    BetlContext *ctx = lua_touserdata(L, lua_upvalueindex(1));
    const char *name = luaL_checkstring(L, 1);
    const char *val  = betl_get_connection(ctx, name);
    if (val) lua_pushstring(L, val); else lua_pushnil(L);
    return 1;
}

static void install_log(lua_State *L, BetlContext *ctx) {
    static const struct { const char *name; int level; } levels[] = {
        { "trace", BETL_LOG_TRACE },
        { "debug", BETL_LOG_DEBUG },
        { "info",  BETL_LOG_INFO  },
        { "warn",  BETL_LOG_WARN  },
        { "error", BETL_LOG_ERROR },
    };
    lua_newtable(L);
    for (size_t i = 0; i < sizeof levels / sizeof levels[0]; ++i) {
        lua_pushlightuserdata(L, ctx);
        lua_pushinteger(L, levels[i].level);
        lua_pushcclosure(L, l_log_at, 2);
        lua_setfield(L, -2, levels[i].name);
    }
    lua_setglobal(L, "log");
}

static void install_params(lua_State *L, BetlContext *ctx) {
    lua_newtable(L);                /* params = {} */
    lua_newtable(L);                /* metatable = {} */
    lua_pushlightuserdata(L, ctx);
    lua_pushcclosure(L, l_params_index, 1);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, l_params_newindex);
    lua_setfield(L, -2, "__newindex");
    lua_setmetatable(L, -2);
    lua_setglobal(L, "params");
}

static void install_connection(lua_State *L, BetlContext *ctx) {
    lua_pushlightuserdata(L, ctx);
    lua_pushcclosure(L, l_connection, 1);
    lua_setglobal(L, "connection");
}


/* ============================================================== *
 *  lua.task component                                              *
 * ============================================================== */

typedef struct {
    BetlContext *ctx;
    char        *script;     /* heap, owned */
    lua_State   *L;
    int          chunk_ref;  /* LUA_REGISTRYINDEX ref to compiled chunk */
} LuaTask;

static int lua_task_init(BetlContext *ctx, const char *cfg_json, void **state) {
    LuaTask *t = calloc(1, sizeof *t);
    if (!t) return BETL_ERR_INTERNAL;
    t->ctx       = ctx;
    t->chunk_ref = LUA_NOREF;

    if (json_get_string(cfg_json, "script", &t->script) != 0 || !t->script) {
        betl_set_error(ctx,
            "lua.task: 'script' is required and must be a string");
        free(t);
        return BETL_ERR_INVALID;
    }

    t->L = luaL_newstate();
    if (!t->L) {
        betl_set_error(ctx, "lua.task: luaL_newstate failed (out of memory)");
        free(t->script);
        free(t);
        return BETL_ERR_INTERNAL;
    }
    luaL_openlibs(t->L);

    install_log(t->L, ctx);
    install_params(t->L, ctx);
    install_connection(t->L, ctx);

    /* Compile but don't execute — fail-fast on syntax errors. */
    int rc = luaL_loadstring(t->L, t->script);
    if (rc != LUA_OK) {
        const char *errm = lua_tostring(t->L, -1);
        betl_set_error(ctx, "lua.task: compile error: %s",
                       errm ? errm : "(unknown)");
        lua_close(t->L);
        free(t->script);
        free(t);
        return BETL_ERR_INVALID;
    }
    t->chunk_ref = luaL_ref(t->L, LUA_REGISTRYINDEX);
    *state = t;
    return BETL_OK;
}

static void lua_task_destroy(void *state) {
    if (!state) return;
    LuaTask *t = state;
    if (t->L) {
        if (t->chunk_ref != LUA_NOREF) {
            luaL_unref(t->L, LUA_REGISTRYINDEX, t->chunk_ref);
        }
        lua_close(t->L);
    }
    free(t->script);
    free(t);
}

static int lua_task_run(void *state) {
    LuaTask *t = state;
    if (!t || !t->L) return BETL_ERR_INTERNAL;

    lua_rawgeti(t->L, LUA_REGISTRYINDEX, t->chunk_ref);
    int rc = lua_pcall(t->L, 0, 0, 0);
    if (rc != LUA_OK) {
        const char *errm = lua_tostring(t->L, -1);
        betl_set_error(t->ctx, "lua.task: %s",
                       errm ? errm : "(unknown error)");
        lua_pop(t->L, 1);
        return BETL_ERR_INTERNAL;
    }
    return BETL_OK;
}


/* ============================================================== *
 *  lua.map component                                               *
 *                                                                  *
 *  TRANSFORM with one input port and one output port. For each     *
 *  input batch we run the user script once per row with `row` as   *
 *  a Lua table populated from the row's columns. The script can    *
 *  mutate the table or return a replacement; the wrap below makes  *
 *  `return row` implicit so users can omit it.                     *
 *                                                                  *
 *  Schema: output = input. Adding new columns from Lua is silently *
 *  ignored. Removing columns (returning a table that lacks one of  *
 *  the input columns) yields a null at that cell.                  *
 *                                                                  *
 *  v0.1 type support: int64 ('l') only. Any other column format    *
 *  on the input schema is rejected at first-batch time.            *
 * ============================================================== */

/* User script gets wrapped as:
 *
 *     return function(row) do <USER> end; return row end
 *
 * The outer chunk runs once at compile time, returning the inner
 * function which we keep referenced via luaL_ref. The `do ... end`
 * keeps any user `local`s scoped, and the trailing `return row`
 * means a script that just mutates row without returning still
 * yields the (mutated) row. If the user explicitly `return`s, that
 * exits the outer function before our trailing return runs. */
static const char LUA_MAP_WRAP_PREFIX[] = "return function(row) do\n";
static const char LUA_MAP_WRAP_SUFFIX[] = "\nend; return row end";

typedef enum {
    LM_T_INT64 = 1,
    LM_T_UTF8  = 2,
    LM_T_BOOL  = 3,
    LM_T_INT8  = 4, LM_T_UINT8  = 5,
    LM_T_INT16 = 6, LM_T_UINT16 = 7,
    LM_T_INT32 = 8, LM_T_UINT32 = 9,
    LM_T_UINT64 = 10,
    LM_T_FLOAT64 = 11,   /* used only by lua.script's output_schema */
} LmType;

/* Per-column output staging. `nulls[i] != 0` means row i is null;
 * the value buffers are then ignored at that position. */
typedef struct {
    LmType   type;
    uint8_t *nulls;       /* length bytes, malloc'd */

    /* INT64 / UINT64 storage. Unsigned shares the buffer; sign is
     * interpretation-only. */
    int64_t *i64_vals;    /* length cells */
    /* Narrow integer storage (signed and unsigned share the buffer). */
    int8_t  *i8_vals;
    int16_t *i16_vals;
    int32_t *i32_vals;

    /* UTF8 storage. Strings appended in row order; offsets[i+1]-offsets[i]
     * is the byte length of row i's value. data is grown geometrically. */
    int32_t *u8_offsets;  /* length+1 cells */
    char    *u8_data;
    size_t   u8_len;
    size_t   u8_cap;

    /* BOOL storage: one byte per row (0/1), packed into a bitmap at
     * finalize. Cheaper to write than a bitmap during the per-row loop. */
    uint8_t *b_vals;
} LmCol;

typedef struct {
    BetlContext             *ctx;
    char                    *script;
    lua_State               *L;
    int                      fn_ref;          /* compiled per-row function */

    struct ArrowArrayStream  input;
    int                      have_input;

    /* Schema cached from upstream on first get_next. Same column set is
     * mirrored on the output. */
    int                      schema_cached;
    size_t                   n_cols;
    char                   **col_names;       /* heap, owned */
    LmType                  *col_types;       /* heap, owned */
    char                     last_err[256];
} LuaMap;

static void luamap_set_err(LuaMap *m, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m->last_err, sizeof m->last_err, fmt, ap);
    va_end(ap);
    betl_set_error(m->ctx, "%s", m->last_err);
}

static int lua_map_init(BetlContext *ctx, const char *cfg_json, void **state) {
    LuaMap *m = calloc(1, sizeof *m);
    if (!m) return BETL_ERR_INTERNAL;
    m->ctx    = ctx;
    m->fn_ref = LUA_NOREF;

    if (json_get_string(cfg_json, "script", &m->script) != 0 || !m->script) {
        betl_set_error(ctx, "lua.map: 'script' is required and must be a string");
        free(m);
        return BETL_ERR_INVALID;
    }

    m->L = luaL_newstate();
    if (!m->L) {
        betl_set_error(ctx, "lua.map: luaL_newstate failed (out of memory)");
        free(m->script); free(m);
        return BETL_ERR_INTERNAL;
    }
    luaL_openlibs(m->L);
    install_log(m->L, ctx);
    install_params(m->L, ctx);
    install_connection(m->L, ctx);

    /* Build the wrapped chunk and compile it. The outer chunk evaluates
     * to the inner function; we run it once and stash the function. */
    size_t plen = strlen(LUA_MAP_WRAP_PREFIX);
    size_t slen = strlen(m->script);
    size_t xlen = strlen(LUA_MAP_WRAP_SUFFIX);
    char *wrapped = malloc(plen + slen + xlen + 1);
    if (!wrapped) {
        lua_close(m->L); free(m->script); free(m);
        return BETL_ERR_INTERNAL;
    }
    memcpy(wrapped,                LUA_MAP_WRAP_PREFIX, plen);
    memcpy(wrapped + plen,         m->script,           slen);
    memcpy(wrapped + plen + slen,  LUA_MAP_WRAP_SUFFIX, xlen);
    wrapped[plen + slen + xlen] = '\0';

    int rc = luaL_loadstring(m->L, wrapped);
    free(wrapped);
    if (rc != LUA_OK) {
        const char *errm = lua_tostring(m->L, -1);
        betl_set_error(ctx, "lua.map: compile error: %s",
                       errm ? errm : "(unknown)");
        lua_close(m->L); free(m->script); free(m);
        return BETL_ERR_INVALID;
    }
    /* Run outer to obtain the inner function on the stack. */
    rc = lua_pcall(m->L, 0, 1, 0);
    if (rc != LUA_OK) {
        const char *errm = lua_tostring(m->L, -1);
        betl_set_error(ctx, "lua.map: function setup failed: %s",
                       errm ? errm : "(unknown)");
        lua_close(m->L); free(m->script); free(m);
        return BETL_ERR_INVALID;
    }
    if (!lua_isfunction(m->L, -1)) {
        betl_set_error(ctx, "lua.map: wrap did not yield a function");
        lua_close(m->L); free(m->script); free(m);
        return BETL_ERR_INVALID;
    }
    m->fn_ref = luaL_ref(m->L, LUA_REGISTRYINDEX);

    *state = m;
    return BETL_OK;
}

static int lua_map_attach_input(void *state, int port,
                                struct ArrowArrayStream *in) {
    (void)port;
    LuaMap *m = state;
    /* Take ownership: copy the stream struct, zero the source so the
     * host can't release it twice. */
    m->input      = *in;
    m->have_input = 1;
    memset(in, 0, sizeof *in);
    return BETL_OK;
}

static void lua_map_destroy(void *state) {
    if (!state) return;
    LuaMap *m = state;
    if (m->have_input && m->input.release) m->input.release(&m->input);
    if (m->L) {
        if (m->fn_ref != LUA_NOREF) {
            luaL_unref(m->L, LUA_REGISTRYINDEX, m->fn_ref);
        }
        lua_close(m->L);
    }
    if (m->col_names) {
        for (size_t i = 0; i < m->n_cols; ++i) free(m->col_names[i]);
        free(m->col_names);
    }
    free(m->col_types);
    free(m->script);
    free(m);
}

/* ---- Output stream callbacks ----------------------------------- */

/* Cache the input schema: record column names and types. Validates
 * that every column is one of the supported leaf types. Idempotent. */
static int luamap_ensure_schema(LuaMap *m) {
    if (m->schema_cached) return 0;
    if (!m->have_input || !m->input.get_schema) {
        luamap_set_err(m, "lua.map: input stream has no get_schema");
        return -1;
    }
    struct ArrowSchema sch = {0};
    if (m->input.get_schema(&m->input, &sch) != 0) {
        luamap_set_err(m, "lua.map: upstream get_schema failed");
        return -1;
    }
    int rc = -1;
    if (!sch.format || strcmp(sch.format, "+s") != 0 || sch.n_children <= 0) {
        luamap_set_err(m, "lua.map: input schema must be a struct with >=1 child");
        goto done;
    }
    size_t n = (size_t)sch.n_children;
    char  **names = calloc(n, sizeof *names);
    LmType *types = calloc(n, sizeof *types);
    if (!names || !types) {
        free(names); free(types);
        luamap_set_err(m, "lua.map: out of memory"); goto done;
    }
    int ok = 1;
    for (size_t i = 0; i < n; ++i) {
        struct ArrowSchema *c = sch.children[i];
        const char *fmt = (c && c->format) ? c->format : NULL;
        if (!fmt) {
            luamap_set_err(m, "lua.map: column %zu has no format", i);
            ok = 0; break;
        }
        if      (strcmp(fmt, "l") == 0) types[i] = LM_T_INT64;
        else if (strcmp(fmt, "L") == 0) types[i] = LM_T_UINT64;
        else if (strcmp(fmt, "c") == 0) types[i] = LM_T_INT8;
        else if (strcmp(fmt, "C") == 0) types[i] = LM_T_UINT8;
        else if (strcmp(fmt, "s") == 0) types[i] = LM_T_INT16;
        else if (strcmp(fmt, "S") == 0) types[i] = LM_T_UINT16;
        else if (strcmp(fmt, "i") == 0) types[i] = LM_T_INT32;
        else if (strcmp(fmt, "I") == 0) types[i] = LM_T_UINT32;
        else if (strcmp(fmt, "u") == 0) types[i] = LM_T_UTF8;
        else {
            luamap_set_err(m,
                "lua.map: column '%s' has unsupported format '%s' "
                "(supported: integer widths c/C/s/S/i/I/l/L; utf8 'u')",
                (c && c->name) ? c->name : "?", fmt);
            ok = 0; break;
        }
        names[i] = strdup(c->name ? c->name : "");
        if (!names[i]) { luamap_set_err(m, "lua.map: out of memory"); ok = 0; break; }
    }
    if (!ok) {
        for (size_t i = 0; i < n; ++i) free(names[i]);
        free(names); free(types);
        goto done;
    }
    m->n_cols        = n;
    m->col_names     = names;
    m->col_types     = types;
    m->schema_cached = 1;
    rc = 0;
done:
    if (sch.release) sch.release(&sch);
    return rc;
}

static int lm_get_schema(struct ArrowArrayStream *st, struct ArrowSchema *out) {
    LuaMap *m = st->private_data;
    if (!m || !m->have_input || !m->input.get_schema) return EINVAL;
    return m->input.get_schema(&m->input, out);
}

/* Per-batch output array release helpers. Each owns the buffers it
 * was constructed with, including the buffers[] pointer array. */
static void release_lm_array_int64_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 2 && arr->buffers) {
        free((void *)arr->buffers[0]);   /* validity bitmap, may be NULL */
        free((void *)arr->buffers[1]);   /* int64 values */
    }
    free(arr->buffers);
    arr->release = NULL;
}

static void release_lm_array_narrow_leaf(struct ArrowArray *arr) {
    /* Same shape as the int64 leaf — keeps int8/16/32 buffers tracked
     * separately in stack traces. */
    if (arr->n_buffers >= 2 && arr->buffers) {
        free((void *)arr->buffers[0]);
        free((void *)arr->buffers[1]);
    }
    free(arr->buffers);
    arr->release = NULL;
}

static void release_lm_array_utf8_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 3 && arr->buffers) {
        free((void *)arr->buffers[0]);   /* validity, may be NULL */
        free((void *)arr->buffers[1]);   /* int32 offsets */
        free((void *)arr->buffers[2]);   /* utf8 data */
    }
    free(arr->buffers);
    arr->release = NULL;
}

static void release_lm_array_bool_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 2 && arr->buffers) {
        free((void *)arr->buffers[0]);   /* validity, may be NULL */
        free((void *)arr->buffers[1]);   /* values bitmap */
    }
    free(arr->buffers);
    arr->release = NULL;
}

static void release_lm_array_struct(struct ArrowArray *arr) {
    for (int64_t i = 0; i < arr->n_children; ++i) {
        if (arr->children[i] && arr->children[i]->release) {
            arr->children[i]->release(arr->children[i]);
        }
        free(arr->children[i]);
    }
    free(arr->children);
    free(arr->buffers);
    arr->release = NULL;
}

/* ---- Cell-level marshal helpers -------------------------------- */

/* Widen any integer leaf (int8/16/32/64, signed or unsigned) onto the
 * Lua stack as a Lua integer. type selects how to interpret the buffer. */
static void lm_push_int_cell(lua_State *L, LmType type,
                             const struct ArrowArray *col,
                             size_t row_idx) {
    size_t row = row_idx + (size_t)col->offset;
    if (col->null_count > 0 && col->buffers[0]) {
        const uint8_t *valid = col->buffers[0];
        if (!(valid[row / 8] & (1u << (row % 8)))) {
            lua_pushnil(L);
            return;
        }
    }
    int64_t v = 0;
    switch (type) {
        case LM_T_INT8:   v = ((const int8_t   *)col->buffers[1])[row]; break;
        case LM_T_UINT8:  v = ((const uint8_t  *)col->buffers[1])[row]; break;
        case LM_T_INT16:  v = ((const int16_t  *)col->buffers[1])[row]; break;
        case LM_T_UINT16: v = ((const uint16_t *)col->buffers[1])[row]; break;
        case LM_T_INT32:  v = ((const int32_t  *)col->buffers[1])[row]; break;
        case LM_T_UINT32: v = ((const uint32_t *)col->buffers[1])[row]; break;
        case LM_T_INT64:
        case LM_T_UINT64: v = ((const int64_t  *)col->buffers[1])[row]; break;
        default: lua_pushnil(L); return;
    }
    lua_pushinteger(L, v);
}

/* Same shape for utf8: pushes the row's bytes as a Lua string
 * (lua strings are 8-bit clean, embedded NUL is fine). */
static void lm_push_utf8_cell(lua_State *L,
                              const struct ArrowArray *col,
                              size_t row_idx) {
    size_t row = row_idx + (size_t)col->offset;
    if (col->null_count > 0 && col->buffers[0]) {
        const uint8_t *valid = col->buffers[0];
        if (!(valid[row / 8] & (1u << (row % 8)))) {
            lua_pushnil(L);
            return;
        }
    }
    const int32_t *offsets = col->buffers[1];
    const char    *data    = col->buffers[2];
    int32_t start = offsets[row];
    int32_t end   = offsets[row + 1];
    lua_pushlstring(L, data + start, (size_t)(end - start));
}

/* ---- Per-column output staging --------------------------------- */

/* Allocate the type-appropriate buffers for `length` rows. */
static int lm_col_init(LmCol *c, LmType type, size_t length) {
    memset(c, 0, sizeof *c);
    c->type  = type;
    c->nulls = calloc(length ? length : 1, sizeof *c->nulls);
    if (!c->nulls) return -1;
    size_t n = length ? length : 1;
    int ok = 0;
    switch (type) {
        case LM_T_INT64: case LM_T_UINT64:
            c->i64_vals = calloc(n, sizeof *c->i64_vals); ok = c->i64_vals != NULL; break;
        case LM_T_INT8: case LM_T_UINT8:
            c->i8_vals  = calloc(n, sizeof *c->i8_vals);  ok = c->i8_vals != NULL;  break;
        case LM_T_INT16: case LM_T_UINT16:
            c->i16_vals = calloc(n, sizeof *c->i16_vals); ok = c->i16_vals != NULL; break;
        case LM_T_INT32: case LM_T_UINT32:
            c->i32_vals = calloc(n, sizeof *c->i32_vals); ok = c->i32_vals != NULL; break;
        case LM_T_UTF8:
            c->u8_offsets = calloc(length + 1, sizeof *c->u8_offsets);
            if (c->u8_offsets) {
                c->u8_cap = 64;
                c->u8_data = malloc(c->u8_cap);
                ok = c->u8_data != NULL;
                if (!ok) { free(c->u8_offsets); c->u8_offsets = NULL; }
            }
            break;
        case LM_T_BOOL:
            c->b_vals = calloc(n, sizeof *c->b_vals); ok = c->b_vals != NULL; break;
        case LM_T_FLOAT64:
            /* lua.map doesn't use float64; lua.script has its own staging. */
            break;
    }
    if (!ok) { free(c->nulls); c->nulls = NULL; return -1; }
    return 0;
}

static void lm_col_free(LmCol *c) {
    free(c->nulls);
    free(c->i64_vals);
    free(c->i8_vals);
    free(c->i16_vals);
    free(c->i32_vals);
    free(c->u8_offsets);
    free(c->u8_data);
    free(c->b_vals);
    memset(c, 0, sizeof *c);
}

static int lm_col_append_string(LmCol *c, const char *s, size_t n,
                                size_t row_idx) {
    /* offsets must already have been written up to row_idx (we always
     * append in row order, so this is the invariant). */
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

/* Finalize a staged column into an ArrowArray leaf. On success the
 * leaf takes ownership of the relevant fields of *c (nulls, vals,
 * offsets, data); we zero them so lm_col_free becomes a no-op. */
static int lm_col_finalize(LmCol *c, size_t length, struct ArrowArray *out) {
    /* Validity bitmap iff any nulls. */
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
    /* nulls[] is no longer needed once we've digested it into vmap. */
    free(c->nulls); c->nulls = NULL;

    if (c->type == LM_T_INT64 || c->type == LM_T_UINT64) {
        const void **bufs = malloc(2 * sizeof *bufs);
        if (!bufs) { free(vmap); return -1; }
        bufs[0] = vmap;
        bufs[1] = c->i64_vals;
        c->i64_vals = NULL;

        out->length     = (int64_t)length;
        out->null_count = null_count;
        out->offset     = 0;
        out->n_buffers  = 2;
        out->n_children = 0;
        out->buffers    = bufs;
        out->release    = release_lm_array_int64_leaf;
        return 0;
    }

    if (c->type == LM_T_INT8 || c->type == LM_T_UINT8 ||
        c->type == LM_T_INT16 || c->type == LM_T_UINT16 ||
        c->type == LM_T_INT32 || c->type == LM_T_UINT32) {
        const void **bufs = malloc(2 * sizeof *bufs);
        if (!bufs) { free(vmap); return -1; }
        bufs[0] = vmap;
        switch (c->type) {
            case LM_T_INT8: case LM_T_UINT8:
                bufs[1] = c->i8_vals;  c->i8_vals = NULL; break;
            case LM_T_INT16: case LM_T_UINT16:
                bufs[1] = c->i16_vals; c->i16_vals = NULL; break;
            case LM_T_INT32: case LM_T_UINT32:
                bufs[1] = c->i32_vals; c->i32_vals = NULL; break;
            default: break;
        }
        out->length     = (int64_t)length;
        out->null_count = null_count;
        out->offset     = 0;
        out->n_buffers  = 2;
        out->n_children = 0;
        out->buffers    = bufs;
        out->release    = release_lm_array_narrow_leaf;
        return 0;
    }

    if (c->type == LM_T_UTF8) {
        /* Smooth offsets across any null rows where lm_col_append_string
         * was never called: they currently sit at 0, but offsets must be
         * monotonically non-decreasing, equal to the previous row's. */
        if (length && c->u8_offsets[length] == 0 && c->u8_len > 0) {
            c->u8_offsets[length] = (int32_t)c->u8_len;
        }
        {
            int32_t last = 0;
            for (size_t i = 1; i <= length; ++i) {
                if (c->u8_offsets[i] < last) c->u8_offsets[i] = last;
                else                          last = c->u8_offsets[i];
            }
        }

        const void **bufs = malloc(3 * sizeof *bufs);
        if (!bufs) { free(vmap); return -1; }
        bufs[0] = vmap;
        bufs[1] = c->u8_offsets;
        bufs[2] = c->u8_data;
        c->u8_offsets = NULL; c->u8_data = NULL;

        out->length     = (int64_t)length;
        out->null_count = null_count;
        out->offset     = 0;
        out->n_buffers  = 3;
        out->n_children = 0;
        out->buffers    = bufs;
        out->release    = release_lm_array_utf8_leaf;
        return 0;
    }

    /* LM_T_BOOL: pack b_vals (one byte per row) into a 1-bit-per-row
     * bitmap matching Arrow's boolean layout. */
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
        bufs[0] = vmap;
        bufs[1] = bitmap;

        out->length     = (int64_t)length;
        out->null_count = null_count;
        out->offset     = 0;
        out->n_buffers  = 2;
        out->n_children = 0;
        out->buffers    = bufs;
        out->release    = release_lm_array_bool_leaf;
        return 0;
    }
}

/* Bail-out helper: free a partial column-staging vector and the input
 * batch. Returns the EIO that lm_get_next propagates. */
static int lm_bail(LuaMap *m, LmCol *cols, size_t n_done,
                   struct ArrowArray *in_arr, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m->last_err, sizeof m->last_err, fmt, ap);
    va_end(ap);
    betl_set_error(m->ctx, "%s", m->last_err);
    if (cols) {
        for (size_t i = 0; i < n_done; ++i) lm_col_free(&cols[i]);
        free(cols);
    }
    if (in_arr && in_arr->release) in_arr->release(in_arr);
    return EIO;
}

static int lm_get_next(struct ArrowArrayStream *st, struct ArrowArray *out) {
    LuaMap *m = st->private_data;
    memset(out, 0, sizeof *out);
    if (!m) return EINVAL;

    if (luamap_ensure_schema(m) != 0) return EIO;

    struct ArrowArray in_arr = {0};
    if (m->input.get_next(&m->input, &in_arr) != 0) {
        const char *e = m->input.get_last_error
                            ? m->input.get_last_error(&m->input) : NULL;
        luamap_set_err(m, "lua.map: upstream get_next failed: %s",
                       e ? e : "(no detail)");
        return EIO;
    }
    if (!in_arr.release) {
        /* End-of-stream: pass through (out already zeroed). */
        return 0;
    }
    if (in_arr.n_children != (int64_t)m->n_cols) {
        size_t expected = m->n_cols;
        long long got = (long long)in_arr.n_children;
        return lm_bail(m, NULL, 0, &in_arr,
            "lua.map: upstream batch has %lld columns, expected %zu",
            got, expected);
    }
    size_t length = (size_t)in_arr.length;

    /* Per-column output staging. */
    LmCol *cols = calloc(m->n_cols, sizeof *cols);
    if (!cols) return lm_bail(m, NULL, 0, &in_arr, "lua.map: out of memory");
    for (size_t c = 0; c < m->n_cols; ++c) {
        if (lm_col_init(&cols[c], m->col_types[c], length) != 0) {
            return lm_bail(m, cols, c, &in_arr, "lua.map: out of memory");
        }
    }

    /* Per-row Lua dispatch. */
    for (size_t r = 0; r < length; ++r) {
        if (betl_should_cancel(m->ctx)) {
            return lm_bail(m, cols, m->n_cols, &in_arr, "lua.map: cancelled");
        }

        /* Push function and row table. */
        lua_rawgeti(m->L, LUA_REGISTRYINDEX, m->fn_ref);
        lua_createtable(m->L, 0, (int)m->n_cols);
        for (size_t c = 0; c < m->n_cols; ++c) {
            switch (m->col_types[c]) {
                case LM_T_INT8: case LM_T_UINT8:
                case LM_T_INT16: case LM_T_UINT16:
                case LM_T_INT32: case LM_T_UINT32:
                case LM_T_INT64: case LM_T_UINT64:
                    lm_push_int_cell(m->L, m->col_types[c], in_arr.children[c], r); break;
                case LM_T_UTF8:  lm_push_utf8_cell (m->L, in_arr.children[c], r); break;
                case LM_T_BOOL:  /* unreachable: schema validation rejects bool input */
                default:         lua_pushnil(m->L); break;
            }
            lua_setfield(m->L, -2, m->col_names[c]);
        }

        int rc = lua_pcall(m->L, 1, 1, 0);
        if (rc != LUA_OK) {
            const char *errm = lua_tostring(m->L, -1);
            char copy[200]; snprintf(copy, sizeof copy, "%s",
                                     errm ? errm : "(unknown)");
            lua_pop(m->L, 1);
            return lm_bail(m, cols, m->n_cols, &in_arr,
                           "lua.map: row %zu: %s", r, copy);
        }
        if (!lua_istable(m->L, -1)) {
            const char *tn = luaL_typename(m->L, -1);
            char copy[64]; snprintf(copy, sizeof copy, "%s", tn);
            lua_pop(m->L, 1);
            return lm_bail(m, cols, m->n_cols, &in_arr,
                "lua.map: row %zu: script must return a table (got %s)",
                r, copy);
        }

        /* Read columns out of the returned table. */
        for (size_t c = 0; c < m->n_cols; ++c) {
            lua_getfield(m->L, -1, m->col_names[c]);
            int t = lua_type(m->L, -1);
            if (t == LUA_TNIL) {
                cols[c].nulls[r] = 1;
                /* For utf8 we still need offsets to advance; leave at
                 * current u8_len, finalize will smooth any gaps. */
                if (cols[c].type == LM_T_UTF8) {
                    cols[c].u8_offsets[r + 1] = (int32_t)cols[c].u8_len;
                }
            } else if (cols[c].type == LM_T_INT8  || cols[c].type == LM_T_UINT8  ||
                       cols[c].type == LM_T_INT16 || cols[c].type == LM_T_UINT16 ||
                       cols[c].type == LM_T_INT32 || cols[c].type == LM_T_UINT32 ||
                       cols[c].type == LM_T_INT64 || cols[c].type == LM_T_UINT64) {
                if (t != LUA_TNUMBER) {
                    const char *tn = lua_typename(m->L, t);
                    char copy[64]; snprintf(copy, sizeof copy, "%s", tn);
                    lua_pop(m->L, 2);
                    return lm_bail(m, cols, m->n_cols, &in_arr,
                        "lua.map: row %zu column '%s': expected integer or nil, got %s",
                        r, m->col_names[c], copy);
                }
                int isint = 0;
                lua_Integer iv = lua_tointegerx(m->L, -1, &isint);
                if (!isint) {
                    lua_pop(m->L, 2);
                    return lm_bail(m, cols, m->n_cols, &in_arr,
                        "lua.map: row %zu column '%s': non-integer number",
                        r, m->col_names[c]);
                }
                int64_t v = (int64_t)iv;
                int64_t lo = INT64_MIN, hi = INT64_MAX;
                const char *tn = "int";
                switch (cols[c].type) {
                    case LM_T_INT8:   lo = INT8_MIN;  hi = INT8_MAX;  tn = "int8";   break;
                    case LM_T_UINT8:  lo = 0; hi = UINT8_MAX;         tn = "uint8";  break;
                    case LM_T_INT16:  lo = INT16_MIN; hi = INT16_MAX; tn = "int16";  break;
                    case LM_T_UINT16: lo = 0; hi = UINT16_MAX;        tn = "uint16"; break;
                    case LM_T_INT32:  lo = INT32_MIN; hi = INT32_MAX; tn = "int32";  break;
                    case LM_T_UINT32: lo = 0; hi = UINT32_MAX;        tn = "uint32"; break;
                    case LM_T_UINT64: lo = 0; hi = INT64_MAX;         tn = "uint64"; break;
                    default: break;
                }
                if (v < lo || v > hi) {
                    lua_pop(m->L, 2);
                    return lm_bail(m, cols, m->n_cols, &in_arr,
                        "lua.map: row %zu column '%s': value %lld out of range for %s",
                        r, m->col_names[c], (long long)v, tn);
                }
                switch (cols[c].type) {
                    case LM_T_INT8: case LM_T_UINT8:    cols[c].i8_vals[r]  = (int8_t)v;  break;
                    case LM_T_INT16: case LM_T_UINT16:  cols[c].i16_vals[r] = (int16_t)v; break;
                    case LM_T_INT32: case LM_T_UINT32:  cols[c].i32_vals[r] = (int32_t)v; break;
                    case LM_T_INT64: case LM_T_UINT64:  cols[c].i64_vals[r] = v;          break;
                    default: break;
                }
            } else { /* LM_T_UTF8 */
                if (t != LUA_TSTRING) {
                    const char *tn = lua_typename(m->L, t);
                    char copy[64]; snprintf(copy, sizeof copy, "%s", tn);
                    lua_pop(m->L, 2);
                    return lm_bail(m, cols, m->n_cols, &in_arr,
                        "lua.map: row %zu column '%s': expected string or nil, got %s",
                        r, m->col_names[c], copy);
                }
                size_t slen = 0;
                const char *s = lua_tolstring(m->L, -1, &slen);
                if (lm_col_append_string(&cols[c], s, slen, r) != 0) {
                    lua_pop(m->L, 2);
                    return lm_bail(m, cols, m->n_cols, &in_arr,
                        "lua.map: row %zu column '%s': out of memory",
                        r, m->col_names[c]);
                }
            }
            lua_pop(m->L, 1);    /* pop the cell */
        }
        lua_pop(m->L, 1);        /* pop the returned table */
    }

    /* Finalize columns into Arrow leaves. */
    struct ArrowArray **kids = calloc(m->n_cols, sizeof *kids);
    if (!kids) {
        return lm_bail(m, cols, m->n_cols, &in_arr, "lua.map: out of memory");
    }
    for (size_t c = 0; c < m->n_cols; ++c) {
        kids[c] = calloc(1, sizeof **kids);
        if (!kids[c] || lm_col_finalize(&cols[c], length, kids[c]) != 0) {
            free(kids[c]);
            for (size_t k = 0; k < c; ++k) {
                if (kids[k]->release) kids[k]->release(kids[k]);
                free(kids[k]);
            }
            free(kids);
            return lm_bail(m, cols, m->n_cols, &in_arr,
                           "lua.map: failed to build output column");
        }
    }
    /* Per-column buffers now belong to the leaves; cols still hold
     * the (now-NULLed) struct shells, plus anything we deliberately
     * left like u8_data when there was nothing to transfer. */
    for (size_t c = 0; c < m->n_cols; ++c) lm_col_free(&cols[c]);
    free(cols);

    const void **outer_bufs = malloc(1 * sizeof *outer_bufs);
    if (!outer_bufs) {
        for (size_t c = 0; c < m->n_cols; ++c) {
            if (kids[c]->release) kids[c]->release(kids[c]);
            free(kids[c]);
        }
        free(kids);
        in_arr.release(&in_arr);
        luamap_set_err(m, "lua.map: out of memory");
        return EIO;
    }
    outer_bufs[0] = NULL;

    out->length     = (int64_t)length;
    out->null_count = 0;
    out->offset     = 0;
    out->n_buffers  = 1;
    out->n_children = (int64_t)m->n_cols;
    out->buffers    = outer_bufs;
    out->children   = kids;
    out->dictionary = NULL;
    out->release    = release_lm_array_struct;

    in_arr.release(&in_arr);
    return 0;
}

static const char *lm_get_last_error(struct ArrowArrayStream *st) {
    LuaMap *m = st->private_data;
    return (m && m->last_err[0]) ? m->last_err : NULL;
}

static void lm_release(struct ArrowArrayStream *st) {
    /* State is owned by lua_map_destroy; here we just mark released. */
    st->private_data = NULL;
    st->release      = NULL;
}

static int lua_map_attach_output(void *state, int port,
                                 struct ArrowArrayStream *out) {
    (void)port;
    LuaMap *m = state;
    out->get_schema     = lm_get_schema;
    out->get_next       = lm_get_next;
    out->get_last_error = lm_get_last_error;
    out->release        = lm_release;
    out->private_data   = m;
    return BETL_OK;
}

static const BetlPortDef lm_inputs[]  = {
    { .name = "in",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "any int64 rows" },
};
static const BetlPortDef lm_outputs[] = {
    { .name = "out", .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "same as in" },
};


/* ============================================================== *
 *  Lua expression engine (SPEC §7)                                 *
 *                                                                  *
 *  An expression like `row.amount > 1000` is wrapped at compile    *
 *  time as:                                                        *
 *      return function(row) return (<expr>) end                    *
 *                                                                  *
 *  The outer chunk runs once and yields the per-row evaluator.     *
 *  evaluate() then calls that function once per row, coerces the   *
 *  Lua result to the Arrow type asked for via `desired_format`,    *
 *  and stages it through the existing LmCol builder.               *
 *                                                                  *
 *  Supported result formats in v0.1:                                *
 *    "l"  int64    — integer or numeric (must round-trip exactly)  *
 *    "u"  utf8     — string; non-strings fall through tostring()   *
 *    "b"  boolean  — Lua truthiness (nil / false → 0, else 1)      *
 *                                                                  *
 *  Supported input column formats: int64 ('l') and utf8 ('u') —    *
 *  the same set lua.map handles. Unsupported types on the input    *
 *  schema are rejected at compile time.                            *
 * ============================================================== */

static const char LUA_EXPR_WRAP_PREFIX[] = "return function(row) return (";
static const char LUA_EXPR_WRAP_SUFFIX[] = ") end";

typedef struct {
    BetlContext *ctx;
    lua_State   *L;
    int          fn_ref;
    size_t       n_cols;
    char       **col_names;
    LmType      *col_types;
    char         last_err[256];
} LuaExpr;

/* Cache the input-schema column names + types into an LuaExpr for
 * later marshaling. Returns -1 on unsupported types. */
static int lua_expr_cache_schema(LuaExpr *e,
                                 const struct ArrowSchema *sch) {
    if (!sch || !sch->format || strcmp(sch->format, "+s") != 0
        || sch->n_children <= 0) {
        snprintf(e->last_err, sizeof e->last_err,
                 "lua-expr: input schema must be a struct with >=1 child");
        betl_set_error(e->ctx, "%s", e->last_err);
        return -1;
    }
    size_t n = (size_t)sch->n_children;
    char  **names = calloc(n, sizeof *names);
    LmType *types = calloc(n, sizeof *types);
    if (!names || !types) {
        free(names); free(types);
        snprintf(e->last_err, sizeof e->last_err, "lua-expr: out of memory");
        betl_set_error(e->ctx, "%s", e->last_err);
        return -1;
    }
    for (size_t i = 0; i < n; ++i) {
        struct ArrowSchema *c = sch->children[i];
        const char *fmt = (c && c->format) ? c->format : NULL;
        if (!fmt) {
            for (size_t k = 0; k < i; ++k) free(names[k]);
            free(names); free(types);
            snprintf(e->last_err, sizeof e->last_err,
                     "lua-expr: column %zu has no format", i);
            betl_set_error(e->ctx, "%s", e->last_err);
            return -1;
        }
        if      (strcmp(fmt, "l") == 0) types[i] = LM_T_INT64;
        else if (strcmp(fmt, "L") == 0) types[i] = LM_T_UINT64;
        else if (strcmp(fmt, "c") == 0) types[i] = LM_T_INT8;
        else if (strcmp(fmt, "C") == 0) types[i] = LM_T_UINT8;
        else if (strcmp(fmt, "s") == 0) types[i] = LM_T_INT16;
        else if (strcmp(fmt, "S") == 0) types[i] = LM_T_UINT16;
        else if (strcmp(fmt, "i") == 0) types[i] = LM_T_INT32;
        else if (strcmp(fmt, "I") == 0) types[i] = LM_T_UINT32;
        else if (strcmp(fmt, "u") == 0) types[i] = LM_T_UTF8;
        else {
            for (size_t k = 0; k < i; ++k) free(names[k]);
            free(names); free(types);
            snprintf(e->last_err, sizeof e->last_err,
                     "lua-expr: input column '%s' has unsupported format '%s'",
                     (c && c->name) ? c->name : "?", fmt);
            betl_set_error(e->ctx, "%s", e->last_err);
            return -1;
        }
        names[i] = strdup(c->name ? c->name : "");
        if (!names[i]) {
            for (size_t k = 0; k < i; ++k) free(names[k]);
            free(names); free(types);
            snprintf(e->last_err, sizeof e->last_err, "lua-expr: out of memory");
            betl_set_error(e->ctx, "%s", e->last_err);
            return -1;
        }
    }
    e->n_cols    = n;
    e->col_names = names;
    e->col_types = types;
    return 0;
}

static int lua_expr_compile(BetlContext *ctx,
                            const char *source,
                            const struct ArrowSchema *input_schema,
                            void **out_handle) {
    if (!source) {
        betl_set_error(ctx, "lua-expr: source is NULL");
        return BETL_ERR_INVALID;
    }
    LuaExpr *e = calloc(1, sizeof *e);
    if (!e) return BETL_ERR_INTERNAL;
    e->ctx    = ctx;
    e->fn_ref = LUA_NOREF;

    if (lua_expr_cache_schema(e, input_schema) != 0) {
        free(e);
        return BETL_ERR_TYPE;
    }

    e->L = luaL_newstate();
    if (!e->L) {
        for (size_t i = 0; i < e->n_cols; ++i) free(e->col_names[i]);
        free(e->col_names); free(e->col_types); free(e);
        betl_set_error(ctx, "lua-expr: luaL_newstate failed");
        return BETL_ERR_INTERNAL;
    }
    luaL_openlibs(e->L);
    install_log(e->L, ctx);
    install_params(e->L, ctx);
    install_connection(e->L, ctx);

    size_t plen = strlen(LUA_EXPR_WRAP_PREFIX);
    size_t slen = strlen(source);
    size_t xlen = strlen(LUA_EXPR_WRAP_SUFFIX);
    char *wrapped = malloc(plen + slen + xlen + 1);
    if (!wrapped) {
        lua_close(e->L);
        for (size_t i = 0; i < e->n_cols; ++i) free(e->col_names[i]);
        free(e->col_names); free(e->col_types); free(e);
        return BETL_ERR_INTERNAL;
    }
    memcpy(wrapped,                LUA_EXPR_WRAP_PREFIX, plen);
    memcpy(wrapped + plen,         source,               slen);
    memcpy(wrapped + plen + slen,  LUA_EXPR_WRAP_SUFFIX, xlen);
    wrapped[plen + slen + xlen] = '\0';

    int rc = luaL_loadstring(e->L, wrapped);
    free(wrapped);
    if (rc != LUA_OK) {
        const char *errm = lua_tostring(e->L, -1);
        betl_set_error(ctx, "lua-expr: compile error: %s",
                       errm ? errm : "(unknown)");
        lua_close(e->L);
        for (size_t i = 0; i < e->n_cols; ++i) free(e->col_names[i]);
        free(e->col_names); free(e->col_types); free(e);
        return BETL_ERR_INVALID;
    }
    if (lua_pcall(e->L, 0, 1, 0) != LUA_OK) {
        const char *errm = lua_tostring(e->L, -1);
        betl_set_error(ctx, "lua-expr: function setup failed: %s",
                       errm ? errm : "(unknown)");
        lua_close(e->L);
        for (size_t i = 0; i < e->n_cols; ++i) free(e->col_names[i]);
        free(e->col_names); free(e->col_types); free(e);
        return BETL_ERR_INVALID;
    }
    if (!lua_isfunction(e->L, -1)) {
        betl_set_error(ctx, "lua-expr: wrap did not yield a function");
        lua_close(e->L);
        for (size_t i = 0; i < e->n_cols; ++i) free(e->col_names[i]);
        free(e->col_names); free(e->col_types); free(e);
        return BETL_ERR_INVALID;
    }
    e->fn_ref = luaL_ref(e->L, LUA_REGISTRYINDEX);

    *out_handle = e;
    return BETL_OK;
}

static void lua_expr_release(void *handle) {
    if (!handle) return;
    LuaExpr *e = handle;
    if (e->L) {
        if (e->fn_ref != LUA_NOREF) {
            luaL_unref(e->L, LUA_REGISTRYINDEX, e->fn_ref);
        }
        lua_close(e->L);
    }
    if (e->col_names) {
        for (size_t i = 0; i < e->n_cols; ++i) free(e->col_names[i]);
        free(e->col_names);
    }
    free(e->col_types);
    free(e);
}

/* Map a desired_format string to LmType; returns -1 if unsupported. */
static int lua_expr_format_to_lmtype(const char *fmt, LmType *out) {
    if (!fmt) return -1;
    if      (strcmp(fmt, "l") == 0) { *out = LM_T_INT64; return 0; }
    else if (strcmp(fmt, "u") == 0) { *out = LM_T_UTF8;  return 0; }
    else if (strcmp(fmt, "b") == 0) { *out = LM_T_BOOL;  return 0; }
    return -1;
}

static int lua_expr_evaluate(void *handle,
                             const struct ArrowArray *input_struct,
                             const char *desired_format,
                             struct ArrowArray *out_array) {
    LuaExpr *e = handle;
    if (!e || !input_struct || !out_array) return BETL_ERR_INVALID;
    memset(out_array, 0, sizeof *out_array);

    LmType out_type;
    if (lua_expr_format_to_lmtype(desired_format, &out_type) != 0) {
        snprintf(e->last_err, sizeof e->last_err,
                 "lua-expr: unsupported desired_format '%s' (v0.1: l, u, b)",
                 desired_format ? desired_format : "(null)");
        betl_set_error(e->ctx, "%s", e->last_err);
        return BETL_ERR_TYPE;
    }
    if (input_struct->n_children != (int64_t)e->n_cols) {
        snprintf(e->last_err, sizeof e->last_err,
                 "lua-expr: input batch has %lld columns, expected %zu",
                 (long long)input_struct->n_children, e->n_cols);
        betl_set_error(e->ctx, "%s", e->last_err);
        return BETL_ERR_TYPE;
    }
    size_t length = (size_t)input_struct->length;

    LmCol col;
    if (lm_col_init(&col, out_type, length) != 0) {
        betl_set_error(e->ctx, "lua-expr: out of memory");
        return BETL_ERR_INTERNAL;
    }

    for (size_t r = 0; r < length; ++r) {
        if (betl_should_cancel(e->ctx)) {
            lm_col_free(&col);
            betl_set_error(e->ctx, "lua-expr: cancelled");
            return BETL_ERR_CANCELLED;
        }
        lua_rawgeti(e->L, LUA_REGISTRYINDEX, e->fn_ref);
        lua_createtable(e->L, 0, (int)e->n_cols);
        for (size_t c = 0; c < e->n_cols; ++c) {
            switch (e->col_types[c]) {
                case LM_T_INT8: case LM_T_UINT8:
                case LM_T_INT16: case LM_T_UINT16:
                case LM_T_INT32: case LM_T_UINT32:
                case LM_T_INT64: case LM_T_UINT64:
                    lm_push_int_cell(e->L, e->col_types[c], input_struct->children[c], r); break;
                case LM_T_UTF8:  lm_push_utf8_cell(e->L, input_struct->children[c], r); break;
                case LM_T_BOOL:
                case LM_T_FLOAT64:
                    /* lua-expr's schema cache rejects these formats at
                     * compile time; defensive nil keeps the switch full. */
                    lua_pushnil(e->L); break;
            }
            lua_setfield(e->L, -2, e->col_names[c]);
        }

        int rc = lua_pcall(e->L, 1, 1, 0);
        if (rc != LUA_OK) {
            const char *errm = lua_tostring(e->L, -1);
            char copy[200]; snprintf(copy, sizeof copy, "%s",
                                     errm ? errm : "(unknown)");
            lua_pop(e->L, 1);
            lm_col_free(&col);
            snprintf(e->last_err, sizeof e->last_err,
                     "lua-expr: row %zu: %s", r, copy);
            betl_set_error(e->ctx, "%s", e->last_err);
            return BETL_ERR_INTERNAL;
        }

        int t = lua_type(e->L, -1);
        if (t == LUA_TNIL) {
            col.nulls[r] = 1;
            if (out_type == LM_T_UTF8) col.u8_offsets[r + 1] = (int32_t)col.u8_len;
        } else if (out_type == LM_T_INT64) {
            if (t != LUA_TNUMBER) {
                const char *tn = lua_typename(e->L, t);
                char copy[64]; snprintf(copy, sizeof copy, "%s", tn);
                lua_pop(e->L, 1);
                lm_col_free(&col);
                snprintf(e->last_err, sizeof e->last_err,
                    "lua-expr: row %zu: expected integer/number, got %s", r, copy);
                betl_set_error(e->ctx, "%s", e->last_err);
                return BETL_ERR_TYPE;
            }
            int isint = 0;
            lua_Integer iv = lua_tointegerx(e->L, -1, &isint);
            if (!isint) {
                lua_pop(e->L, 1);
                lm_col_free(&col);
                snprintf(e->last_err, sizeof e->last_err,
                    "lua-expr: row %zu: number does not fit in int64", r);
                betl_set_error(e->ctx, "%s", e->last_err);
                return BETL_ERR_TYPE;
            }
            col.i64_vals[r] = (int64_t)iv;
        } else if (out_type == LM_T_UTF8) {
            /* Coerce anything stringable. lua_tolstring on a number
             * mutates the stack value to its string form; that's fine
             * here since we're about to pop it. */
            size_t slen = 0;
            const char *s = NULL;
            if (t == LUA_TSTRING || t == LUA_TNUMBER) {
                s = lua_tolstring(e->L, -1, &slen);
            }
            if (!s) {
                const char *tn = lua_typename(e->L, t);
                char copy[64]; snprintf(copy, sizeof copy, "%s", tn);
                lua_pop(e->L, 1);
                lm_col_free(&col);
                snprintf(e->last_err, sizeof e->last_err,
                    "lua-expr: row %zu: cannot coerce %s to utf8", r, copy);
                betl_set_error(e->ctx, "%s", e->last_err);
                return BETL_ERR_TYPE;
            }
            if (lm_col_append_string(&col, s, slen, r) != 0) {
                lua_pop(e->L, 1);
                lm_col_free(&col);
                betl_set_error(e->ctx, "lua-expr: out of memory");
                return BETL_ERR_INTERNAL;
            }
        } else { /* LM_T_BOOL */
            /* Lua truthiness: only nil and false are false. */
            col.b_vals[r] = (uint8_t)(lua_toboolean(e->L, -1) ? 1 : 0);
        }
        lua_pop(e->L, 1);
    }

    if (lm_col_finalize(&col, length, out_array) != 0) {
        lm_col_free(&col);
        betl_set_error(e->ctx, "lua-expr: failed to finalize output column");
        return BETL_ERR_INTERNAL;
    }
    lm_col_free(&col);
    return BETL_OK;
}

static const BetlExprEngine lua_expr_engine = {
    .lang     = "lua",
    .compile  = lua_expr_compile,
    .evaluate = lua_expr_evaluate,
    .release  = lua_expr_release,
};


/* ============================================================== *
 *  lua.script — async TRANSFORM                                    *
 *                                                                  *
 *  Compared to lua.map (synchronous, 1 input row → 1 output row),  *
 *  lua.script can fan in / fan out arbitrarily: a stateful script  *
 *  with explicit emit() calls. The SSIS asynchronous Script        *
 *  Component equivalent.                                           *
 *                                                                  *
 *  YAML:                                                           *
 *    - id: smooth                                                  *
 *      type: lua.script                                            *
 *      from: source                                                *
 *      output_schema:                                              *
 *        - { name: id,       type: l }                             *
 *        - { name: smoothed, type: g }                             *
 *      script: |                                                   *
 *        local window = {}                                         *
 *        function on_row(row)                                      *
 *          table.insert(window, row.value)                         *
 *          if #window > 10 then table.remove(window, 1) end        *
 *          if #window == 10 then                                   *
 *            local s = 0                                           *
 *            for _, v in ipairs(window) do s = s + v end           *
 *            emit({ id = row.id, smoothed = s / 10 })              *
 *          end                                                     *
 *        end                                                       *
 *        function on_eof()  -- optional final flush                *
 *        end                                                       *
 *                                                                  *
 *  Supported `type:` (Arrow format chars): l (int64), g (float64), *
 *  u (utf8), b (bool). Narrow widths are an add-on for v0.2.       *
 * ============================================================== */

/* Per-output-column growable staging. Mirrors LmCol but uses a `cap`
 * field so emit() can append without knowing batch length in advance. */
typedef struct {
    LmType   type;
    uint8_t *nulls;          /* cap bytes */
    int64_t *i64_vals;       /* cap cells (INT64) */
    double  *f64_vals;       /* cap cells (FLOAT64) */
    uint8_t *b_vals;         /* cap cells (BOOL, packed at finalize) */
    int32_t *u8_offsets;     /* cap+1 cells (UTF8) */
    char    *u8_data;
    size_t   u8_len;
    size_t   u8_cap;
    size_t   cap;            /* rows reserved */
} ScCol;

typedef struct {
    BetlContext             *ctx;
    char                    *script;
    lua_State               *L;
    int                      fn_on_row_ref;
    int                      fn_on_eof_ref;

    struct ArrowArrayStream  input;
    int                      have_input;

    /* Declared output schema (parsed from YAML config). */
    size_t                   n_out;
    char                   **out_names;
    LmType                  *out_types;

    /* Per-batch staging — grown geometrically by emit. */
    ScCol                   *out_cols;
    size_t                   n_buffered;        /* rows currently in out_cols */
    int                      emit_failed;       /* one row's emit had a type error */
    char                     emit_err[256];

    /* Upstream schema cache, populated on first get_next batch. */
    int                      _in_cached;
    size_t                   _in_n;
    char                   **_in_names;
    LmType                  *_in_types;

    int                      eof_seen;
    char                     last_err[256];
} LuaScript;

static void lscript_set_err(LuaScript *s, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(s->last_err, sizeof s->last_err, fmt, ap); va_end(ap);
    betl_set_error(s->ctx, "%s", s->last_err);
}

/* --- output_schema parsing -------------------------------------- */

/* Walk a top-level JSON array, calling cb for each element. Each element
 * substring starts at `value` with `value_len` bytes — same callback shape
 * as the runtime's tx walker. Adapted for the lua provider's local needs.
 * Returns 0 on success, -1 on malformed input. */
typedef int (*sc_arr_visit_fn)(const char *value, size_t value_len, void *user);

static const char *sc_skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    return p;
}

static const char *sc_skip_value(const char *p) {
    p = sc_skip_ws(p);
    if (!*p) return NULL;
    if (*p == '{' || *p == '[') {
        char open = *p, close = (open == '{') ? '}' : ']';
        int depth = 0;
        for (; *p; ++p) {
            if (*p == '"') {
                ++p;
                while (*p && *p != '"') { if (*p == '\\' && p[1]) ++p; ++p; }
                if (!*p) return NULL;
                continue;
            }
            if (*p == open)  ++depth;
            else if (*p == close) {
                --depth;
                if (depth == 0) return p + 1;
            }
        }
        return NULL;
    }
    if (*p == '"') {
        ++p;
        while (*p && *p != '"') { if (*p == '\\' && p[1]) ++p; ++p; }
        return *p ? p + 1 : NULL;
    }
    /* number / true / false / null — read until separator. */
    while (*p && *p != ',' && *p != ']' && *p != '}'
           && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') ++p;
    return p;
}

static int sc_walk_array(const char *p, sc_arr_visit_fn cb, void *user) {
    p = sc_skip_ws(p);
    if (*p != '[') return -1;
    ++p;
    p = sc_skip_ws(p);
    if (*p == ']') return 0;
    for (;;) {
        p = sc_skip_ws(p);
        const char *start = p;
        const char *end = sc_skip_value(p);
        if (!end) return -1;
        if (cb(start, (size_t)(end - start), user) != 0) return -1;
        p = sc_skip_ws(end);
        if (*p == ',') { ++p; continue; }
        if (*p == ']') return 0;
        return -1;
    }
}

typedef struct {
    LuaScript *s;
    int        err;
} OutSchemaCtx;

static int parse_out_col(const char *value, size_t value_len, void *user) {
    OutSchemaCtx *c = user;
    LuaScript *s = c->s;
    if (value_len == 0 || value[0] != '{') { c->err = 1; return -1; }
    char *vbuf = malloc(value_len + 1);
    if (!vbuf) { c->err = 1; return -1; }
    memcpy(vbuf, value, value_len);
    vbuf[value_len] = '\0';

    char *name = NULL, *type = NULL;
    json_get_string(vbuf, "name", &name);
    json_get_string(vbuf, "type", &type);
    free(vbuf);
    if (!name || !type) {
        free(name); free(type);
        c->err = 1; return -1;
    }

    LmType lt;
    if (strcmp(type, "l") == 0)      lt = LM_T_INT64;
    else if (strcmp(type, "g") == 0) lt = LM_T_FLOAT64;
    else if (strcmp(type, "u") == 0) lt = LM_T_UTF8;
    else if (strcmp(type, "b") == 0) lt = LM_T_BOOL;
    else {
        lscript_set_err(s,
            "lua.script: output column '%s' has unsupported type '%s' "
            "(v1: l, g, u, b)", name, type);
        free(name); free(type);
        c->err = 1; return -1;
    }
    free(type);

    /* Realloc one at a time, attaching each result to the state before
     * the next call so a partial-success cleanup path frees the right
     * pointers (s->out_names is the caller's source of truth). */
    char **gn = realloc(s->out_names, (s->n_out + 1) * sizeof *gn);
    if (!gn) { free(name); c->err = 1; return -1; }
    s->out_names = gn;
    LmType *gt = realloc(s->out_types, (s->n_out + 1) * sizeof *gt);
    if (!gt) { free(name); c->err = 1; return -1; }
    s->out_types = gt;
    s->out_names[s->n_out] = name;
    s->out_types[s->n_out] = lt;
    ++s->n_out;
    return 0;
}

/* --- ScCol storage --------------------------------------------- */

static void sc_col_init(ScCol *c, LmType t) {
    memset(c, 0, sizeof *c);
    c->type = t;
}

static void sc_col_free(ScCol *c) {
    free(c->nulls);
    free(c->i64_vals);
    free(c->f64_vals);
    free(c->b_vals);
    free(c->u8_offsets);
    free(c->u8_data);
    memset(c, 0, sizeof *c);
}

static int sc_col_reserve(ScCol *c, size_t want) {
    if (want <= c->cap) return 0;
    size_t nc = c->cap ? c->cap : 64;
    while (nc < want) nc *= 2;
    uint8_t *nn = realloc(c->nulls, nc);
    if (!nn) return -1;
    c->nulls = nn;
    memset(c->nulls + c->cap, 0, nc - c->cap);
    switch (c->type) {
        case LM_T_INT64: {
            int64_t *p = realloc(c->i64_vals, nc * sizeof *p);
            if (!p) return -1;
            c->i64_vals = p;
            break;
        }
        case LM_T_FLOAT64: {
            double *p = realloc(c->f64_vals, nc * sizeof *p);
            if (!p) return -1;
            c->f64_vals = p;
            break;
        }
        case LM_T_BOOL: {
            uint8_t *p = realloc(c->b_vals, nc);
            if (!p) return -1;
            c->b_vals = p;
            break;
        }
        case LM_T_UTF8: {
            int32_t *po = realloc(c->u8_offsets, (nc + 1) * sizeof *po);
            if (!po) return -1;
            c->u8_offsets = po;
            if (c->cap == 0) c->u8_offsets[0] = 0;
            if (c->u8_cap == 0) {
                c->u8_cap = 128;
                c->u8_data = malloc(c->u8_cap);
                if (!c->u8_data) return -1;
            }
            break;
        }
        default: return -1;
    }
    c->cap = nc;
    return 0;
}

/* --- emit() Lua C function ------------------------------------- */

/* Reads a Lua table on top of the stack; stages one row of cells into
 * each output column. Sets s->emit_failed + s->emit_err on type error. */
static int sc_stage_row(LuaScript *s, lua_State *L, int tbl_idx) {
    size_t row = s->n_buffered;
    for (size_t c = 0; c < s->n_out; ++c) {
        ScCol *col = &s->out_cols[c];
        if (sc_col_reserve(col, row + 1) != 0) {
            snprintf(s->emit_err, sizeof s->emit_err, "out of memory");
            s->emit_failed = 1; return -1;
        }
        lua_getfield(L, tbl_idx, s->out_names[c]);
        int t = lua_type(L, -1);
        if (t == LUA_TNIL) {
            col->nulls[row] = 1;
            if (col->type == LM_T_UTF8) col->u8_offsets[row + 1] = (int32_t)col->u8_len;
            lua_pop(L, 1);
            continue;
        }
        col->nulls[row] = 0;
        switch (col->type) {
            case LM_T_INT64: {
                if (t != LUA_TNUMBER) {
                    snprintf(s->emit_err, sizeof s->emit_err,
                        "column '%s' expects integer, got %s",
                        s->out_names[c], lua_typename(L, t));
                    s->emit_failed = 1; lua_pop(L, 1); return -1;
                }
                int isint = 0;
                lua_Integer iv = lua_tointegerx(L, -1, &isint);
                if (!isint) {
                    snprintf(s->emit_err, sizeof s->emit_err,
                        "column '%s' expects integer; got non-integer number",
                        s->out_names[c]);
                    s->emit_failed = 1; lua_pop(L, 1); return -1;
                }
                col->i64_vals[row] = (int64_t)iv;
                break;
            }
            case LM_T_FLOAT64: {
                if (t != LUA_TNUMBER) {
                    snprintf(s->emit_err, sizeof s->emit_err,
                        "column '%s' expects number, got %s",
                        s->out_names[c], lua_typename(L, t));
                    s->emit_failed = 1; lua_pop(L, 1); return -1;
                }
                col->f64_vals[row] = (double)lua_tonumber(L, -1);
                break;
            }
            case LM_T_BOOL: {
                col->b_vals[row] = (uint8_t)(lua_toboolean(L, -1) ? 1 : 0);
                break;
            }
            case LM_T_UTF8: {
                size_t slen = 0;
                const char *str = NULL;
                if (t == LUA_TSTRING || t == LUA_TNUMBER) {
                    str = lua_tolstring(L, -1, &slen);
                }
                if (!str) {
                    snprintf(s->emit_err, sizeof s->emit_err,
                        "column '%s' expects string, got %s",
                        s->out_names[c], lua_typename(L, t));
                    s->emit_failed = 1; lua_pop(L, 1); return -1;
                }
                size_t need = col->u8_len + slen;
                if (need > col->u8_cap) {
                    size_t nc = col->u8_cap;
                    while (nc < need) nc *= 2;
                    char *nd = realloc(col->u8_data, nc);
                    if (!nd) {
                        snprintf(s->emit_err, sizeof s->emit_err, "out of memory");
                        s->emit_failed = 1; lua_pop(L, 1); return -1;
                    }
                    col->u8_data = nd; col->u8_cap = nc;
                }
                if (slen) memcpy(col->u8_data + col->u8_len, str, slen);
                col->u8_len += slen;
                col->u8_offsets[row + 1] = (int32_t)col->u8_len;
                break;
            }
            default:
                snprintf(s->emit_err, sizeof s->emit_err,
                    "internal: unhandled output type for '%s'", s->out_names[c]);
                s->emit_failed = 1; lua_pop(L, 1); return -1;
        }
        lua_pop(L, 1);
    }
    ++s->n_buffered;
    return 0;
}

/* upvalue 1: LuaScript* */
static int l_emit(lua_State *L) {
    LuaScript *s = lua_touserdata(L, lua_upvalueindex(1));
    if (!lua_istable(L, 1)) {
        return luaL_error(L, "emit: expected a table");
    }
    if (sc_stage_row(s, L, 1) != 0) {
        /* Surface the staged error back into Lua so the script's
         * pcall catches it (the host can then bail the batch). */
        return luaL_error(L, "emit: %s", s->emit_err);
    }
    return 0;
}

/* --- lifecycle ------------------------------------------------- */

static int lua_script_init(BetlContext *ctx, const char *cfg_json, void **state) {
    LuaScript *s = calloc(1, sizeof *s);
    if (!s) return BETL_ERR_INTERNAL;
    s->ctx = ctx;
    s->fn_on_row_ref = LUA_NOREF;
    s->fn_on_eof_ref = LUA_NOREF;

    if (json_get_string(cfg_json, "script", &s->script) != 0 || !s->script) {
        betl_set_error(ctx, "lua.script: 'script' is required");
        free(s); return BETL_ERR_INVALID;
    }
    const char *os = json_value_after(cfg_json, "output_schema");
    if (!os || *os != '[') {
        betl_set_error(ctx, "lua.script: 'output_schema' is required");
        free(s->script); free(s); return BETL_ERR_INVALID;
    }
    OutSchemaCtx oc = { .s = s };
    if (sc_walk_array(os, parse_out_col, &oc) != 0 || oc.err) {
        if (s->last_err[0] == '\0')
            betl_set_error(ctx, "lua.script: malformed 'output_schema'");
        for (size_t i = 0; i < s->n_out; ++i) free(s->out_names[i]);
        free(s->out_names); free(s->out_types); free(s->script); free(s);
        return BETL_ERR_INVALID;
    }
    if (s->n_out == 0) {
        betl_set_error(ctx, "lua.script: 'output_schema' is empty");
        free(s->script); free(s); return BETL_ERR_INVALID;
    }

    /* Allocate per-output-column staging buffers. */
    s->out_cols = calloc(s->n_out, sizeof *s->out_cols);
    if (!s->out_cols) {
        for (size_t i = 0; i < s->n_out; ++i) free(s->out_names[i]);
        free(s->out_names); free(s->out_types); free(s->script); free(s);
        return BETL_ERR_INTERNAL;
    }
    for (size_t i = 0; i < s->n_out; ++i) sc_col_init(&s->out_cols[i], s->out_types[i]);

    s->L = luaL_newstate();
    if (!s->L) {
        for (size_t i = 0; i < s->n_out; ++i) free(s->out_names[i]);
        free(s->out_cols); free(s->out_names); free(s->out_types);
        free(s->script); free(s);
        betl_set_error(ctx, "lua.script: luaL_newstate failed");
        return BETL_ERR_INTERNAL;
    }
    luaL_openlibs(s->L);
    install_log(s->L, ctx);
    install_params(s->L, ctx);
    install_connection(s->L, ctx);

    /* Register `emit` as a global C closure carrying this state. */
    lua_pushlightuserdata(s->L, s);
    lua_pushcclosure(s->L, l_emit, 1);
    lua_setglobal(s->L, "emit");

    /* Run the script once (top level) to define on_row / on_eof globals. */
    if (luaL_loadstring(s->L, s->script) != LUA_OK) {
        const char *e = lua_tostring(s->L, -1);
        betl_set_error(ctx, "lua.script: compile error: %s", e ? e : "(unknown)");
        goto fail;
    }
    if (lua_pcall(s->L, 0, 0, 0) != LUA_OK) {
        const char *e = lua_tostring(s->L, -1);
        betl_set_error(ctx, "lua.script: top-level error: %s", e ? e : "(unknown)");
        goto fail;
    }

    lua_getglobal(s->L, "on_row");
    if (!lua_isfunction(s->L, -1)) {
        lua_pop(s->L, 1);
        betl_set_error(ctx, "lua.script: script must define a global function on_row");
        goto fail;
    }
    s->fn_on_row_ref = luaL_ref(s->L, LUA_REGISTRYINDEX);

    lua_getglobal(s->L, "on_eof");
    if (lua_isfunction(s->L, -1)) {
        s->fn_on_eof_ref = luaL_ref(s->L, LUA_REGISTRYINDEX);
    } else {
        lua_pop(s->L, 1);     /* on_eof is optional */
    }

    *state = s;
    return BETL_OK;

fail:
    if (s->L) lua_close(s->L);
    for (size_t i = 0; i < s->n_out; ++i) {
        free(s->out_names[i]);
        sc_col_free(&s->out_cols[i]);
    }
    free(s->out_cols); free(s->out_names); free(s->out_types);
    free(s->script); free(s);
    return BETL_ERR_INVALID;
}

static int lua_script_attach_input(void *state, int port,
                                   struct ArrowArrayStream *in) {
    (void)port;
    LuaScript *s = state;
    s->input = *in;
    s->have_input = 1;
    memset(in, 0, sizeof *in);
    return BETL_OK;
}

static void lua_script_destroy(void *state) {
    if (!state) return;
    LuaScript *s = state;
    if (s->have_input && s->input.release) s->input.release(&s->input);
    if (s->L) {
        if (s->fn_on_row_ref != LUA_NOREF) luaL_unref(s->L, LUA_REGISTRYINDEX, s->fn_on_row_ref);
        if (s->fn_on_eof_ref != LUA_NOREF) luaL_unref(s->L, LUA_REGISTRYINDEX, s->fn_on_eof_ref);
        lua_close(s->L);
    }
    if (s->out_cols) {
        for (size_t i = 0; i < s->n_out; ++i) sc_col_free(&s->out_cols[i]);
        free(s->out_cols);
    }
    if (s->out_names) {
        for (size_t i = 0; i < s->n_out; ++i) free(s->out_names[i]);
        free(s->out_names);
    }
    free(s->out_types);
    if (s->_in_names) {
        for (size_t i = 0; i < s->_in_n; ++i) free(s->_in_names[i]);
        free(s->_in_names);
    }
    free(s->_in_types);
    free(s->script);
    free(s);
}

/* --- output ArrowSchema --------------------------------------- */

static void sc_release_schema_named(struct ArrowSchema *sch) {
    free((void *)sch->name);
    sch->name = NULL;
    sch->release = NULL;
}

static void sc_release_schema_struct(struct ArrowSchema *sch) {
    for (int64_t i = 0; i < sch->n_children; ++i) {
        if (sch->children[i]) {
            if (sch->children[i]->release) sch->children[i]->release(sch->children[i]);
            free(sch->children[i]);
        }
    }
    free(sch->children);
    sch->release = NULL;
}

static int lua_script_get_schema(struct ArrowArrayStream *st,
                                 struct ArrowSchema *out) {
    LuaScript *s = st->private_data;
    memset(out, 0, sizeof *out);
    if (!s) return EINVAL;
    struct ArrowSchema **kids = calloc(s->n_out, sizeof *kids);
    if (!kids) return ENOMEM;
    for (size_t i = 0; i < s->n_out; ++i) {
        struct ArrowSchema *c = calloc(1, sizeof *c);
        char *nm = strdup(s->out_names[i]);
        if (!c || !nm) {
            free(c); free(nm);
            for (size_t k = 0; k < i; ++k) {
                if (kids[k]->release) kids[k]->release(kids[k]);
                free(kids[k]);
            }
            free(kids); return ENOMEM;
        }
        c->name    = nm;
        c->flags   = ARROW_FLAG_NULLABLE;
        c->release = sc_release_schema_named;
        switch (s->out_types[i]) {
            case LM_T_INT64:   c->format = "l"; break;
            case LM_T_FLOAT64: c->format = "g"; break;
            case LM_T_UTF8:    c->format = "u"; break;
            case LM_T_BOOL:    c->format = "b"; break;
            default:           c->format = "u"; break;
        }
        kids[i] = c;
    }
    out->format     = "+s";
    out->n_children = (int64_t)s->n_out;
    out->children   = kids;
    out->release    = sc_release_schema_struct;
    return 0;
}

/* --- output ArrowArray builders ------------------------------- */

static void sc_release_leaf2(struct ArrowArray *arr) {
    if (arr->n_buffers >= 2 && arr->buffers) {
        free((void *)arr->buffers[0]);
        free((void *)arr->buffers[1]);
    }
    free(arr->buffers);
    arr->release = NULL;
}
static void sc_release_leaf3(struct ArrowArray *arr) {
    if (arr->n_buffers >= 3 && arr->buffers) {
        free((void *)arr->buffers[0]);
        free((void *)arr->buffers[1]);
        free((void *)arr->buffers[2]);
    }
    free(arr->buffers);
    arr->release = NULL;
}
static void sc_release_struct(struct ArrowArray *arr) {
    for (int64_t i = 0; i < arr->n_children; ++i) {
        if (arr->children[i]) {
            if (arr->children[i]->release) arr->children[i]->release(arr->children[i]);
            free(arr->children[i]);
        }
    }
    free(arr->children);
    free(arr->buffers);
    arr->release = NULL;
}

/* Build a validity bitmap iff any nulls. Returns NULL with *out_nc=0
 * if none, or a fresh malloc otherwise. */
static uint8_t *sc_build_validity(const uint8_t *nulls, size_t n,
                                  int64_t *out_nc) {
    int64_t nc = 0;
    for (size_t i = 0; i < n; ++i) if (nulls[i]) ++nc;
    *out_nc = nc;
    if (nc == 0) return NULL;
    size_t bytes = (n + 7) / 8;
    uint8_t *vmap = malloc(bytes ? bytes : 1);
    if (!vmap) return NULL;
    memset(vmap, 0xFF, bytes ? bytes : 1);
    for (size_t i = 0; i < n; ++i) {
        if (nulls[i]) vmap[i / 8] &= (uint8_t)~(1u << (i % 8));
    }
    return vmap;
}

static int sc_finalize_col(ScCol *c, size_t n, struct ArrowArray *out) {
    int64_t null_count = 0;
    uint8_t *vmap = sc_build_validity(c->nulls, n, &null_count);
    if (null_count > 0 && !vmap) return -1;
    free(c->nulls); c->nulls = NULL;

    if (c->type == LM_T_INT64) {
        int64_t *vals = c->i64_vals; c->i64_vals = NULL;
        const void **bufs = malloc(2 * sizeof *bufs);
        if (!bufs) { free(vmap); free(vals); return -1; }
        bufs[0] = vmap; bufs[1] = vals;
        out->length = (int64_t)n; out->null_count = null_count;
        out->n_buffers = 2; out->buffers = bufs;
        out->release = sc_release_leaf2;
        return 0;
    }
    if (c->type == LM_T_FLOAT64) {
        double *vals = c->f64_vals; c->f64_vals = NULL;
        const void **bufs = malloc(2 * sizeof *bufs);
        if (!bufs) { free(vmap); free(vals); return -1; }
        bufs[0] = vmap; bufs[1] = vals;
        out->length = (int64_t)n; out->null_count = null_count;
        out->n_buffers = 2; out->buffers = bufs;
        out->release = sc_release_leaf2;
        return 0;
    }
    if (c->type == LM_T_BOOL) {
        size_t bytes = (n + 7) / 8;
        uint8_t *bm = calloc(bytes ? bytes : 1, 1);
        if (!bm) { free(vmap); return -1; }
        for (size_t i = 0; i < n; ++i) {
            if (c->b_vals[i]) bm[i / 8] |= (uint8_t)(1u << (i % 8));
        }
        free(c->b_vals); c->b_vals = NULL;
        const void **bufs = malloc(2 * sizeof *bufs);
        if (!bufs) { free(vmap); free(bm); return -1; }
        bufs[0] = vmap; bufs[1] = bm;
        out->length = (int64_t)n; out->null_count = null_count;
        out->n_buffers = 2; out->buffers = bufs;
        out->release = sc_release_leaf2;
        return 0;
    }
    /* LM_T_UTF8: smooth offsets across nulls before emitting. */
    int32_t last = 0;
    for (size_t i = 1; i <= n; ++i) {
        if (c->u8_offsets[i] < last) c->u8_offsets[i] = last;
        else last = c->u8_offsets[i];
    }
    int32_t *offs = c->u8_offsets; c->u8_offsets = NULL;
    char    *data = c->u8_data;    c->u8_data = NULL;
    if (!data) { data = malloc(1); if (!data) { free(vmap); free(offs); return -1; } }
    const void **bufs = malloc(3 * sizeof *bufs);
    if (!bufs) { free(vmap); free(offs); free(data); return -1; }
    bufs[0] = vmap; bufs[1] = offs; bufs[2] = data;
    out->length = (int64_t)n; out->null_count = null_count;
    out->n_buffers = 3; out->buffers = bufs;
    out->release = sc_release_leaf3;
    return 0;
}

/* Build one struct ArrowArray from current buffered rows, then reset
 * the per-column staging so emit() starts fresh. */
static int sc_flush_batch(LuaScript *s, struct ArrowArray *out) {
    size_t n = s->n_buffered;
    struct ArrowArray **kids = calloc(s->n_out, sizeof *kids);
    if (!kids) { lscript_set_err(s, "lua.script: OOM"); return -1; }
    for (size_t i = 0; i < s->n_out; ++i) {
        kids[i] = calloc(1, sizeof **kids);
        if (!kids[i] || sc_finalize_col(&s->out_cols[i], n, kids[i]) != 0) {
            for (size_t k = 0; k <= i; ++k) {
                if (kids[k]) {
                    if (kids[k]->release) kids[k]->release(kids[k]);
                    free(kids[k]);
                }
            }
            free(kids);
            lscript_set_err(s, "lua.script: finalize column %zu failed", i);
            return -1;
        }
    }
    const void **outer = malloc(1 * sizeof *outer);
    if (!outer) {
        for (size_t i = 0; i < s->n_out; ++i) {
            if (kids[i]->release) kids[i]->release(kids[i]);
            free(kids[i]);
        }
        free(kids); lscript_set_err(s, "lua.script: OOM");
        return -1;
    }
    outer[0] = NULL;
    out->length     = (int64_t)n;
    out->null_count = 0;
    out->n_buffers  = 1;
    out->n_children = (int64_t)s->n_out;
    out->buffers    = outer;
    out->children   = kids;
    out->release    = sc_release_struct;
    s->n_buffered = 0;
    for (size_t i = 0; i < s->n_out; ++i) sc_col_init(&s->out_cols[i], s->out_types[i]);
    return 0;
}

/* --- input handling ------------------------------------------- */

/* Push the row_idx-th row of a struct-of-leaves Arrow array onto the
 * Lua stack as a table, keyed by upstream column names. Mirrors how
 * lua.map builds the per-row table but threads through the schema we
 * just cached on `in_col_*` arrays. */
static void sc_push_row(LuaScript *s, const struct ArrowArray *in_arr,
                        size_t r, char **in_names, LmType *in_types, size_t n_in) {
    lua_createtable(s->L, 0, (int)n_in);
    for (size_t c = 0; c < n_in; ++c) {
        switch (in_types[c]) {
            case LM_T_INT8: case LM_T_UINT8:
            case LM_T_INT16: case LM_T_UINT16:
            case LM_T_INT32: case LM_T_UINT32:
            case LM_T_INT64: case LM_T_UINT64:
                lm_push_int_cell(s->L, in_types[c], in_arr->children[c], r); break;
            case LM_T_UTF8:
                lm_push_utf8_cell(s->L, in_arr->children[c], r); break;
            default:
                lua_pushnil(s->L); break;
        }
        lua_setfield(s->L, -2, in_names[c]);
    }
}

/* Cache upstream schema names + types onto the LuaScript state. */
static int sc_cache_input_schema_self(LuaScript *s) {
    if (s->_in_cached) return 0;
    struct ArrowSchema up = {0};
    if (!s->have_input || !s->input.get_schema
        || s->input.get_schema(&s->input, &up) != 0) {
        lscript_set_err(s, "lua.script: upstream get_schema failed");
        return -1;
    }
    int rc = -1;
    if (!up.format || strcmp(up.format, "+s") != 0 || up.n_children <= 0) {
        lscript_set_err(s, "lua.script: input must be a struct"); goto done;
    }
    size_t n = (size_t)up.n_children;
    char  **names = calloc(n, sizeof *names);
    LmType *types = calloc(n, sizeof *types);
    if (!names || !types) {
        free(names); free(types);
        lscript_set_err(s, "lua.script: OOM"); goto done;
    }
    for (size_t i = 0; i < n; ++i) {
        struct ArrowSchema *cs = up.children[i];
        const char *fmt = (cs && cs->format) ? cs->format : NULL;
        if (!fmt) { lscript_set_err(s, "lua.script: column %zu missing format", i); goto cleanup; }
        LmType t;
        if      (!strcmp(fmt, "l")) t = LM_T_INT64;
        else if (!strcmp(fmt, "L")) t = LM_T_UINT64;
        else if (!strcmp(fmt, "c")) t = LM_T_INT8;
        else if (!strcmp(fmt, "C")) t = LM_T_UINT8;
        else if (!strcmp(fmt, "s")) t = LM_T_INT16;
        else if (!strcmp(fmt, "S")) t = LM_T_UINT16;
        else if (!strcmp(fmt, "i")) t = LM_T_INT32;
        else if (!strcmp(fmt, "I")) t = LM_T_UINT32;
        else if (!strcmp(fmt, "u")) t = LM_T_UTF8;
        else {
            lscript_set_err(s,
                "lua.script: input column '%s' has unsupported format '%s' "
                "(supported: integer widths, utf8)",
                (cs && cs->name) ? cs->name : "?", fmt);
            goto cleanup;
        }
        names[i] = strdup(cs->name ? cs->name : "");
        if (!names[i]) { lscript_set_err(s, "lua.script: OOM"); goto cleanup; }
        types[i] = t;
    }
    s->_in_names = names; s->_in_types = types; s->_in_n = n;
    s->_in_cached = 1;
    rc = 0; goto done;
cleanup:
    for (size_t i = 0; i < n; ++i) free(names[i]);
    free(names); free(types);
done:
    if (up.release) up.release(&up);
    return rc;
}

/* --- get_next ------------------------------------------------- */

static int lua_script_get_next(struct ArrowArrayStream *st,
                               struct ArrowArray *out) {
    LuaScript *s = st->private_data;
    memset(out, 0, sizeof *out);
    if (!s) return EINVAL;

    /* Pull batches and run on_row until at least one output row is
     * buffered, or upstream is exhausted (call on_eof then). */
    while (s->n_buffered == 0) {
        if (s->eof_seen) {
            return 0;     /* terminal: empty out, release NULL */
        }
        struct ArrowArray in_arr = {0};
        if (s->input.get_next(&s->input, &in_arr) != 0) {
            lscript_set_err(s, "lua.script: upstream get_next failed");
            return EIO;
        }
        if (!in_arr.release) {
            /* End of stream — fire on_eof if present. */
            s->eof_seen = 1;
            if (s->fn_on_eof_ref != LUA_NOREF) {
                lua_rawgeti(s->L, LUA_REGISTRYINDEX, s->fn_on_eof_ref);
                if (lua_pcall(s->L, 0, 0, 0) != LUA_OK) {
                    const char *e = lua_tostring(s->L, -1);
                    lscript_set_err(s, "lua.script: on_eof error: %s", e ? e : "(unknown)");
                    lua_pop(s->L, 1);
                    return EIO;
                }
            }
            if (s->n_buffered == 0) return 0;  /* nothing flushed at EOF */
            break;
        }
        /* Cache the input schema once we see a real batch. */
        if (!s->_in_cached) {
            if (sc_cache_input_schema_self(s) != 0) {
                in_arr.release(&in_arr);
                return EIO;
            }
        }
        size_t length = (size_t)in_arr.length;
        for (size_t r = 0; r < length; ++r) {
            if (betl_should_cancel(s->ctx)) {
                in_arr.release(&in_arr);
                lscript_set_err(s, "lua.script: cancelled");
                return EIO;
            }
            lua_rawgeti(s->L, LUA_REGISTRYINDEX, s->fn_on_row_ref);
            sc_push_row(s, &in_arr, r, s->_in_names, s->_in_types, s->_in_n);
            if (lua_pcall(s->L, 1, 0, 0) != LUA_OK) {
                const char *e = lua_tostring(s->L, -1);
                lscript_set_err(s, "lua.script: on_row row=%zu: %s",
                                r, e ? e : "(unknown)");
                lua_pop(s->L, 1);
                in_arr.release(&in_arr);
                return EIO;
            }
        }
        in_arr.release(&in_arr);
    }
    if (s->n_buffered == 0) return 0;
    if (sc_flush_batch(s, out) != 0) return EIO;
    return 0;
}

static const char *lua_script_get_last_error(struct ArrowArrayStream *st) {
    LuaScript *s = st->private_data;
    return (s && s->last_err[0]) ? s->last_err : NULL;
}

static void lua_script_release(struct ArrowArrayStream *st) {
    st->private_data = NULL;
    st->release = NULL;
}

static int lua_script_attach_output(void *state, int port,
                                    struct ArrowArrayStream *out) {
    (void)port;
    LuaScript *s = state;
    out->get_schema     = lua_script_get_schema;
    out->get_next       = lua_script_get_next;
    out->get_last_error = lua_script_get_last_error;
    out->release        = lua_script_release;
    out->private_data   = s;
    return BETL_OK;
}

static const BetlPortDef ls_inputs[]  = {
    { .name = "in",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "rows to process" },
};
static const BetlPortDef ls_outputs[] = {
    { .name = "out", .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "emitted rows" },
};


/* ============================================================== *
 *  Provider entry                                                  *
 * ============================================================== */

static const BetlComponentDef components[] = {
    { .name               = "lua.task",
      .kind               = BETL_KIND_TASK,
      .config_schema_json = "{\"type\":\"object\","
                             "\"properties\":{\"script\":{\"type\":\"string\"}},"
                             "\"required\":[\"script\"]}",
      .flags              = 0,
      .init               = lua_task_init,
      .destroy            = lua_task_destroy,
      .task_run           = lua_task_run },

    { .name               = "lua.map",
      .kind               = BETL_KIND_TRANSFORM,
      .config_schema_json = "{\"type\":\"object\","
                             "\"properties\":{\"script\":{\"type\":\"string\"}},"
                             "\"required\":[\"script\"]}",
      .flags              = 0,
      .inputs             = lm_inputs,
      .input_count        = 1,
      .outputs            = lm_outputs,
      .output_count       = 1,
      .init               = lua_map_init,
      .destroy            = lua_map_destroy,
      .attach_input       = lua_map_attach_input,
      .attach_output      = lua_map_attach_output },

    { .name               = "lua.script",
      .kind               = BETL_KIND_TRANSFORM,
      .config_schema_json = "{}",
      .flags              = 0,
      .inputs             = ls_inputs,
      .input_count        = 1,
      .outputs            = ls_outputs,
      .output_count       = 1,
      .init               = lua_script_init,
      .destroy            = lua_script_destroy,
      .attach_input       = lua_script_attach_input,
      .attach_output      = lua_script_attach_output },
};

static const BetlProvider lua_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-lua",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = components,
    .component_count = sizeof components / sizeof components[0],
    .expr_engine     = &lua_expr_engine,
};

BETL_EXPORT const BetlProvider *betl_provider_entry(void) {
    return &lua_provider;
}
