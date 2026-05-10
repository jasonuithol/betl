/*
 * betl-provider.h — Public ABI for betl provider plugins
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 the Better ETL contributors.
 *
 * A betl provider is a shared library (.so / .dylib / .dll) that exports
 * one symbol — `betl_provider_entry` — and contributes one or more
 * components (sources, transforms, sinks, or control-flow tasks) to the
 * engine.
 *
 * Data crosses the boundary as Apache Arrow arrays and Arrow Array
 * Streams. The Arrow C Data Interface and Arrow C Stream Interface
 * structs are declared verbatim in this header so plugins do NOT need
 * to depend on libarrow at compile or link time. They are bit-for-bit
 * compatible with the canonical layout published by the Arrow project,
 * and the standard `ARROW_C_DATA_INTERFACE` / `ARROW_C_STREAM_INTERFACE`
 * include guards are honoured so that code which DOES pull in libarrow
 * later will not double-declare them.
 *
 * Minimum supported language standard: C11.
 *
 * --- Minimal "hello world" source ----------------------------------------
 *
 *   static int hello_init(BetlContext *ctx, const char *cfg_json,
 *                         void **state)            { *state = NULL; return 0; }
 *   static int hello_attach_output(void *state, int port,
 *                                  struct ArrowArrayStream *out) {
 *       // populate out->get_schema, out->get_next, out->release...
 *       return 0;
 *   }
 *   static void hello_destroy(void *state)         { (void)state; }
 *
 *   static const BetlPortDef hello_outputs[] = {
 *       { .name = "out", .schema_mode = BETL_SCHEMA_STATIC, .schema_json =
 *         "{\"fields\":[{\"name\":\"msg\",\"type\":\"string\"}]}" },
 *   };
 *
 *   static const BetlComponentDef hello_components[] = {
 *       { .name = "hello.greet", .kind = BETL_KIND_SOURCE,
 *         .config_schema_json = "{}",
 *         .outputs = hello_outputs, .output_count = 1,
 *         .init = hello_init,
 *         .attach_output = hello_attach_output,
 *         .destroy = hello_destroy },
 *   };
 *
 *   static const BetlProvider hello_provider = {
 *       .abi_version    = BETL_ABI_VERSION,
 *       .name           = "betl-hello",
 *       .version        = "0.1.0",
 *       .license        = "Apache-2.0",
 *       .components     = hello_components,
 *       .component_count = sizeof(hello_components)/sizeof(hello_components[0]),
 *   };
 *
 *   BETL_EXPORT const BetlProvider *betl_provider_entry(void) {
 *       return &hello_provider;
 *   }
 */

#ifndef BETL_PROVIDER_H
#define BETL_PROVIDER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * ABI versioning
 * =========================================================================
 *
 * The host refuses to load any provider whose `abi_version` does not
 * match `BETL_ABI_VERSION` exactly while we are pre-1.0. After 1.0 we
 * will switch to a major.minor scheme where the host accepts any minor
 * within the same major.
 */
#define BETL_ABI_VERSION 1u


/* =========================================================================
 * Symbol export
 * =========================================================================
 */
#if defined(_WIN32) || defined(__CYGWIN__)
#  define BETL_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#  define BETL_EXPORT __attribute__((visibility("default")))
#else
#  define BETL_EXPORT
#endif


/* =========================================================================
 * Arrow C Data Interface (verbatim, Apache 2.0, from apache/arrow)
 * =========================================================================
 */
#ifndef ARROW_C_DATA_INTERFACE
#define ARROW_C_DATA_INTERFACE

#define ARROW_FLAG_DICTIONARY_ORDERED   1
#define ARROW_FLAG_NULLABLE             2
#define ARROW_FLAG_MAP_KEYS_SORTED      4

struct ArrowSchema {
    /* Array type description */
    const char *format;
    const char *name;
    const char *metadata;
    int64_t flags;
    int64_t n_children;
    struct ArrowSchema **children;
    struct ArrowSchema *dictionary;

    /* Release callback. Producer sets; consumer calls when done. */
    void (*release)(struct ArrowSchema *);
    /* Opaque producer-specific data */
    void *private_data;
};

struct ArrowArray {
    /* Array data description */
    int64_t length;
    int64_t null_count;
    int64_t offset;
    int64_t n_buffers;
    int64_t n_children;
    const void **buffers;
    struct ArrowArray **children;
    struct ArrowArray *dictionary;

    /* Release callback. Producer sets; consumer calls when done. */
    void (*release)(struct ArrowArray *);
    /* Opaque producer-specific data */
    void *private_data;
};

#endif /* ARROW_C_DATA_INTERFACE */


/* =========================================================================
 * Arrow C Stream Interface (verbatim, Apache 2.0, from apache/arrow)
 * =========================================================================
 */
#ifndef ARROW_C_STREAM_INTERFACE
#define ARROW_C_STREAM_INTERFACE

struct ArrowArrayStream {
    /* Get the stream's schema. Returns 0 on success, errno-like on failure. */
    int (*get_schema)(struct ArrowArrayStream *, struct ArrowSchema *out);

    /* Get the next array. On end-of-stream, fills `out` with a released
     * (i.e. NULL release) array and returns 0. Returns errno-like on failure. */
    int (*get_next)(struct ArrowArrayStream *, struct ArrowArray *out);

    /* Optional human-readable description of the last error. May return NULL. */
    const char *(*get_last_error)(struct ArrowArrayStream *);

    /* Release callback. Producer sets; consumer calls when done. */
    void (*release)(struct ArrowArrayStream *);

    /* Opaque producer-specific data */
    void *private_data;
};

#endif /* ARROW_C_STREAM_INTERFACE */


/* =========================================================================
 * Status codes
 * =========================================================================
 *
 * All ABI functions that return `int` return 0 on success and a positive
 * status code on failure. The status code is informational; the human-
 * readable error is set via `betl_set_error` (see BetlContext).
 */
typedef enum {
    BETL_OK             = 0,
    BETL_ERR_INVALID    = 1,   /* invalid configuration or argument */
    BETL_ERR_IO         = 2,   /* underlying I/O failed */
    BETL_ERR_TYPE       = 3,   /* schema / type mismatch */
    BETL_ERR_NOT_FOUND  = 4,   /* connection / resource not found */
    BETL_ERR_AUTH       = 5,   /* authentication / authorization failed */
    BETL_ERR_CANCELLED  = 6,   /* host requested cancellation */
    BETL_ERR_INTERNAL   = 7,   /* component bug */
    BETL_ERR_UNSUPPORTED= 8,   /* feature not implemented in this provider */
    BETL_ERR_LAST       = 255  /* upper bound; do not use */
} BetlStatus;


/* =========================================================================
 * Logging
 * =========================================================================
 */
typedef enum {
    BETL_LOG_TRACE = 10,
    BETL_LOG_DEBUG = 20,
    BETL_LOG_INFO  = 30,
    BETL_LOG_WARN  = 40,
    BETL_LOG_ERROR = 50
} BetlLogLevel;


/* =========================================================================
 * Component kinds
 * =========================================================================
 *
 * SOURCE     — outputs only, drives the start of a data flow.
 * TRANSFORM  — both inputs and outputs.
 * SINK       — inputs only, terminates a data flow with a side effect.
 * TASK       — control-flow step (sql.execute, shell, http, etc.). No
 *              streaming I/O. Runs once via `task_run`.
 */
typedef enum {
    BETL_KIND_SOURCE    = 1,
    BETL_KIND_TRANSFORM = 2,
    BETL_KIND_SINK      = 3,
    BETL_KIND_TASK      = 4
} BetlComponentKind;


/* =========================================================================
 * Component flags
 * =========================================================================
 */
typedef enum {
    /* The component's vtable methods may be called concurrently from
     * multiple host threads on the same `state`. Default is single-threaded. */
    BETL_FLAG_THREADSAFE      = 1u << 0,

    /* The component is deterministic given its inputs and config. The host
     * may use this for caching / replay decisions. */
    BETL_FLAG_DETERMINISTIC   = 1u << 1,

    /* The component participates in transactional / exactly-once delivery
     * (via `txn_begin` / `txn_commit` / `txn_abort`, see optional vtable). */
    BETL_FLAG_TRANSACTIONAL   = 1u << 2
} BetlComponentFlags;


/* =========================================================================
 * Port description
 * =========================================================================
 */
typedef enum {
    /* Schema is fully known from the static `schema_json`. */
    BETL_SCHEMA_STATIC  = 1,
    /* Schema can be derived from config at validate-time via
     * `describe_output` / `validate_input`. */
    BETL_SCHEMA_DERIVED = 2,
    /* Schema is only known once the component is running (e.g. arbitrary
     * SQL query). The host validates downstream compatibility at runtime. */
    BETL_SCHEMA_DYNAMIC = 3
} BetlSchemaMode;

typedef struct {
    const char     *name;          /* port identifier (stable across versions) */
    BetlSchemaMode  schema_mode;
    const char     *schema_json;   /* JSON of the Arrow schema, if STATIC */
    const char     *doc;           /* optional one-line description */
} BetlPortDef;


/* =========================================================================
 * Host context
 * =========================================================================
 *
 * BetlContext is opaque. Components interact with the host (logging,
 * cancellation, error reporting) through the function-pointer table that
 * the host hands them at init time, accessible via the helper functions
 * declared below. The host guarantees the context outlives the component.
 */
typedef struct BetlContext BetlContext;

/* Logging — host-provided. Format string is printf-style. */
void betl_log(BetlContext *ctx, BetlLogLevel level,
              const char *fmt, ...);

/* Set a human-readable error message for the most recent failure. The
 * host owns the storage; the component's string is copied. May be called
 * from any vtable method. */
void betl_set_error(BetlContext *ctx, const char *fmt, ...);

/* Returns nonzero if the host has requested cancellation (e.g. user
 * pressed Ctrl-C, sibling component failed and `on_failure: stop` is
 * in force). Long-running loops MUST poll this. */
int  betl_should_cancel(BetlContext *ctx);

/* Look up a parameter or pipeline-wide setting by dotted path.
 * Returns NULL if absent. The returned string is owned by the host
 * and valid for the lifetime of the run. */
const char *betl_get_param(BetlContext *ctx, const char *path);

/* Resolve a connection reference (e.g. "warehouse") to a JSON config
 * blob containing the resolved DSN, credentials, etc. Returns NULL if
 * the connection name is unknown. The returned string is owned by the
 * host. Secrets in the returned JSON are real values, not placeholders;
 * components MUST NOT log or persist them. */
const char *betl_get_connection(BetlContext *ctx, const char *name);

/* Look up a loaded expression engine by language tag (e.g. "lua",
 * "literal"). Returns NULL if no such engine is loaded. The pointer is
 * owned by the host registry and remains valid for the lifetime of the
 * run. Used by core transforms (filter / map) and by any provider that
 * wants to evaluate user expressions itself. */
struct BetlExprEngine;
const struct BetlExprEngine *betl_get_expr_engine(BetlContext *ctx,
                                                  const char *lang);


/* =========================================================================
 * Component vtable
 * =========================================================================
 *
 * Lifecycle (data-flow components):
 *
 *   1. `init(ctx, config_json, &state)`
 *        — parse config, allocate state. Called once, on the thread that
 *          will own the component.
 *
 *   2. For each input port:  `attach_input (state, port, *in_stream)`
 *      For each output port: `attach_output(state, port, *out_stream)`
 *        — `in_stream` is already populated by the upstream component;
 *          the component stores it and pulls from it later.
 *        — `out_stream` is uninitialized; the component fills in its
 *          callback table so the downstream consumer can pull.
 *
 *   3. The host pulls from each terminal sink, which transitively pulls
 *      through the graph until sources return end-of-stream.
 *
 *   4. `destroy(state)` — free everything. Called exactly once even on
 *      error paths.
 *
 * Lifecycle (TASK components):
 *
 *   1. `init(ctx, config_json, &state)`
 *   2. `task_run(state)` — runs to completion or cancellation; returns
 *      0 on success, BETL_ERR_* on failure.
 *   3. `destroy(state)`
 *
 * Optional methods may be NULL. Required methods are noted below.
 */
typedef struct {
    /* Stable identifier as referenced from a pipeline (`type: postgres.upsert`).
     * Component names are global; provider authors should namespace. */
    const char         *name;

    BetlComponentKind   kind;

    /* JSON Schema (draft 2020-12) describing the YAML config block this
     * component accepts. Used by `betl validate` and tooling. */
    const char         *config_schema_json;

    /* Bitwise OR of BetlComponentFlags. */
    uint32_t            flags;

    /* Port descriptors. NULL/0 if the component has no ports of that
     * direction (e.g. SOURCE has zero inputs, SINK has zero outputs,
     * TASK has zero of both). */
    const BetlPortDef  *inputs;
    size_t              input_count;
    const BetlPortDef  *outputs;
    size_t              output_count;

    /* --- Required: lifecycle ------------------------------------------- */

    /* Initialize. `config_json` is the YAML config block for this step,
     * already validated against `config_schema_json`, converted to JSON.
     * On success, set `*state` to a heap-allocated handle the host will
     * pass back to all subsequent calls. Returns BetlStatus. */
    int  (*init)(BetlContext *ctx, const char *config_json, void **state);

    /* Free `state` and any resources. Called exactly once per init. */
    void (*destroy)(void *state);

    /* --- Required for SOURCE / TRANSFORM ------------------------------- */

    /* Populate `*out_stream` so the downstream consumer can pull. The
     * component owns `*out_stream`'s private_data and must release it
     * via the stream's release callback. Returns BetlStatus. */
    int  (*attach_output)(void *state, int port_idx,
                          struct ArrowArrayStream *out_stream);

    /* --- Required for TRANSFORM / SINK --------------------------------- */

    /* Receive an upstream stream the component will pull from. The
     * component MUST eventually call `in_stream->release(in_stream)`,
     * either during destroy or sooner. Returns BetlStatus. */
    int  (*attach_input)(void *state, int port_idx,
                         struct ArrowArrayStream *in_stream);

    /* --- Required for SINK --------------------------------------------- */

    /* Drain all input streams to the sink's underlying destination.
     * Blocks until end-of-stream on every input or until the host
     * requests cancellation. Returns BetlStatus. */
    int  (*sink_run)(void *state);

    /* --- Required for TASK --------------------------------------------- */

    /* Execute the task to completion. Returns BetlStatus. */
    int  (*task_run)(void *state);

    /* --- Optional: schema discovery (validate-time) -------------------- */

    /* Given a parsed config, fill `*out` with the Arrow schema this
     * component will produce on the named output port. Used when
     * `schema_mode == BETL_SCHEMA_DERIVED`. May allocate; the caller
     * releases via `out->release(out)`. Returns BetlStatus. */
    int  (*describe_output)(BetlContext *ctx, const char *config_json,
                            int port_idx, struct ArrowSchema *out);

    /* Validate that the given input schema is acceptable on the named
     * input port. Returns BETL_OK if compatible, BETL_ERR_TYPE otherwise
     * (with details via betl_set_error). Used when an upstream component
     * has a known schema and we want to fail fast at validate-time. */
    int  (*validate_input)(BetlContext *ctx, const char *config_json,
                           int port_idx, const struct ArrowSchema *in);

    /* --- Optional: transactions (BETL_FLAG_TRANSACTIONAL) -------------- */

    int  (*txn_begin) (void *state);
    int  (*txn_commit)(void *state);
    int  (*txn_abort) (void *state);

    /* --- Optional: dynamic output port lookup --------------------------- *
     *
     * Components with multiple, configuration-determined output ports
     * (canonical example: `conditional_split`) implement this to map a
     * user-supplied port name from `from: step:port_name` into a port
     * index suitable for `attach_output`. Returns the index on success,
     * -1 if the name is not recognized.
     *
     * Components with a single, statically-named output don't need this:
     * the host treats an empty / missing port suffix as port 0, and a
     * named suffix that matches `outputs[0].name` as port 0.
     *
     * Called after init() and before any attach_output() — the state is
     * fully populated. Must be idempotent and side-effect-free; the host
     * may call it multiple times for the same name.
     */
    int  (*output_port_index)(void *state, const char *port_name);

    /* Reserved for forward compatibility. Must be zero-initialized. */
    void *_reserved[7];
} BetlComponentDef;


/* =========================================================================
 * Expression engine (SPEC §7)
 * =========================================================================
 *
 * An expression engine is a small bolt-on a provider can advertise to
 * supply the language behind YAML `expr:` / `where:` blocks. It is
 * orthogonal to ordinary components: a provider may expose components
 * only, an engine only, or both (e.g. betl-lua exposes lua.task,
 * lua.map AND the "lua" engine).
 *
 * Lifecycle:
 *
 *   1. compile(ctx, source, input_schema, &handle)
 *        — parse / load / type-check the expression once for a given
 *          input schema. The host may call compile multiple times on
 *          the same engine for different expressions; the handles are
 *          independent.
 *
 *   2. evaluate(handle, input_struct, desired_format, *out_array)
 *        — once per upstream batch, write a single leaf array of the
 *          requested format containing the per-row results. The host
 *          owns *out_array's release callback after this returns.
 *          `desired_format` is one of the Arrow leaf format strings
 *          ("l", "u", "b", "g", ...). The engine SHOULD coerce its
 *          natural result type to `desired_format` where reasonable
 *          and SHOULD return BETL_ERR_TYPE if it cannot.
 *
 *   3. release(handle)
 *        — free everything. Called exactly once per compile.
 *
 * Engines are looked up by `lang`. Loading two engines that claim the
 * same lang is a registry-load error.
 */
/* Forward-declared above for betl_get_expr_engine. Defined here. */
struct BetlExprEngine {
    /* Language tag ("lua", "python", "sql", ...). MUST be a stable,
     * non-NULL string for the lifetime of the loaded library. */
    const char *lang;

    int  (*compile)(BetlContext *ctx,
                    const char *source,
                    const struct ArrowSchema *input_schema,
                    void **handle);

    int  (*evaluate)(void *handle,
                     const struct ArrowArray *input_struct,
                     const char *desired_format,
                     struct ArrowArray *out_array);

    void (*release)(void *handle);

    /* Reserved for forward compatibility. Must be zero-initialized. */
    void *_reserved[4];
};
typedef struct BetlExprEngine BetlExprEngine;


/* =========================================================================
 * Provider top-level
 * =========================================================================
 */
typedef struct {
    /* Must equal BETL_ABI_VERSION at compile time. Host refuses to load
     * providers with a mismatched ABI. */
    uint32_t                  abi_version;

    /* Provider identity. `name` should match the shared-library filename
     * (`betl-postgres.so` -> "betl-postgres") for diagnostics. */
    const char               *name;
    const char               *version;       /* freeform, e.g. "0.3.1" */

    /* SPDX license identifier of the provider itself. The host displays
     * this in `betl providers` so users know what they're running. */
    const char               *license;

    /* Optional provider-wide init / shutdown hooks, called once when the
     * library is loaded / unloaded. Both may be NULL. Use these for
     * one-time setup like `curl_global_init` or `OpenSSL_init`, NOT for
     * per-pipeline state. */
    int  (*provider_init)(void);
    void (*provider_shutdown)(void);

    /* Static array of components contributed by this provider. */
    const BetlComponentDef   *components;
    size_t                    component_count;

    /* Optional expression engine. NULL if this provider doesn't supply
     * one. See BetlExprEngine above. */
    const BetlExprEngine     *expr_engine;

    /* Reserved for forward compatibility. Must be zero-initialized. */
    void                     *_reserved[7];
} BetlProvider;


/* =========================================================================
 * Entry point
 * =========================================================================
 *
 * Every provider library MUST export this symbol with C linkage and
 * default visibility. It is the only symbol the host looks up. The
 * returned pointer must remain valid for the lifetime of the loaded
 * library (i.e. `static const`).
 */
BETL_EXPORT const BetlProvider *betl_provider_entry(void);


#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* BETL_PROVIDER_H */
