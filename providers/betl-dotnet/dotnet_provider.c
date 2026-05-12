/* betl-dotnet — C# / VB.NET task and script components (v0.2).
 *
 * Compile-once model:
 *   1. At init we hash (user source + lang + shim ABI version).
 *   2. Look for ~/.cache/betl/dotnet/<hash>.so. If present, dlopen.
 *   3. Otherwise materialise a temp build dir with the shim project
 *      + user source, shell out to `dotnet publish -p:PublishAot=true
 *      -p:NativeLib=Shared`, copy the resulting .so into the cache.
 *   4. dlopen, resolve `betl_dotnet_init` and the kind-specific entry
 *      point, init the bridges, hand off.
 *
 * SDK discovery (first-match):
 *   1. $BETL_DOTNET_ROOT/dotnet
 *   2. <provider .so directory>/../../../deps/dotnet/dotnet
 *      (in-tree development layout: build/providers/betl-dotnet/ .so
 *       sibling to deps/dotnet/)
 *   3. `dotnet` in PATH
 *
 * Shim source discovery (same first-match style):
 *   1. $BETL_DOTNET_SHIM_DIR
 *   2. <provider .so directory>/../../../providers/betl-dotnet/shim
 *   3. <provider .so directory>/shim   (installed layout)
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE              /* asprintf, mkdtemp */

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "betl/provider.h"

/* Bump when the C↔C# ABI in shim/ changes. Cached artifacts compiled
 * against an older ABI become invalid (the hash flips). */
#define BETL_DOTNET_SHIM_ABI_VERSION "1"


/* ============================================================== *
 *  JSON value extraction (string-search shortcut; same approach   *
 *  as the lua provider)                                            *
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

static int json_decode_string(const char *p, char **out) {
    *out = NULL;
    if (!p || *p != '"') return -1;
    ++p;
    size_t cap = strlen(p) + 1;
    char *buf = malloc(cap);
    if (!buf) return -1;
    size_t i = 0;
    while (*p && *p != '"') {
        if (*p != '\\') { buf[i++] = *p++; continue; }
        ++p;
        switch (*p) {
            case '"':  buf[i++] = '"';  ++p; break;
            case '\\': buf[i++] = '\\'; ++p; break;
            case '/':  buf[i++] = '/';  ++p; break;
            case 'n':  buf[i++] = '\n'; ++p; break;
            case 't':  buf[i++] = '\t'; ++p; break;
            case 'r':  buf[i++] = '\r'; ++p; break;
            case 'b':  buf[i++] = '\b'; ++p; break;
            case 'f':  buf[i++] = '\f'; ++p; break;
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
 *  Path / discovery                                                *
 * ============================================================== */

static int file_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

static int file_executable(const char *path) {
    return path && access(path, X_OK) == 0;
}

/* Return the directory containing this loaded plugin .so, or NULL on
 * failure. The caller owns the returned string. dladdr() looks up the
 * Dl_info for a known symbol in this object — `json_value_after` works.
 *
 * The function-pointer-to-void* conversion is undefined in ISO C but
 * required by POSIX dladdr; the union trick suppresses the pedantic
 * warning while keeping the bytes intact. */
static char *plugin_so_dir(void) {
    Dl_info info = {0};
    union { const char *(*fn)(const char *, const char *); void *obj; } pun;
    pun.fn = json_value_after;
    if (!dladdr(pun.obj, &info) || !info.dli_fname) return NULL;
    char *copy = strdup(info.dli_fname);
    if (!copy) return NULL;
    char *slash = strrchr(copy, '/');
    if (slash) *slash = '\0';
    return copy;
}

/* Look for the dotnet binary. Tries:
 *   1. $BETL_DOTNET_ROOT/dotnet
 *   2. <so dir>/../../../deps/dotnet/dotnet   (in-tree layout)
 *   3. `dotnet` resolved on PATH
 * Returns a malloc'd path or NULL. */
static char *find_dotnet(void) {
    const char *env = getenv("BETL_DOTNET_ROOT");
    if (env && *env) {
        char *p = NULL;
        if (asprintf(&p, "%s/dotnet", env) > 0 && file_executable(p)) return p;
        free(p);
    }

    char *so_dir = plugin_so_dir();
    if (so_dir) {
        char *p = NULL;
        if (asprintf(&p, "%s/../../../deps/dotnet/dotnet", so_dir) > 0
            && file_executable(p)) {
            free(so_dir);
            return p;
        }
        free(p);
        free(so_dir);
    }

    /* PATH fallback — search the PATH ourselves rather than relying on
     * the loader, which doesn't add anything we'd care about. */
    const char *path = getenv("PATH");
    if (!path) return NULL;
    char *path_copy = strdup(path);
    if (!path_copy) return NULL;
    char *result = NULL;
    for (char *tok = strtok(path_copy, ":"); tok; tok = strtok(NULL, ":")) {
        char *p = NULL;
        if (asprintf(&p, "%s/dotnet", tok) > 0 && file_executable(p)) {
            result = p;
            break;
        }
        free(p);
    }
    free(path_copy);
    return result;
}

/* Same hunt-pattern for the shim source directory. */
static char *find_shim_dir(void) {
    const char *env = getenv("BETL_DOTNET_SHIM_DIR");
    if (env && *env && file_exists(env)) return strdup(env);

    char *so_dir = plugin_so_dir();
    if (!so_dir) return NULL;
    const char *candidates[] = {
        "/../../../providers/betl-dotnet/shim",   /* build-tree layout */
        "/shim",                                  /* installed alongside .so */
        NULL,
    };
    for (int i = 0; candidates[i]; ++i) {
        char *p = NULL;
        if (asprintf(&p, "%s%s", so_dir, candidates[i]) > 0 && file_exists(p)) {
            free(so_dir);
            return p;
        }
        free(p);
    }
    free(so_dir);
    return NULL;
}

static char *cache_dir(void) {
    const char *xdg = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    char *p = NULL;
    if (xdg && *xdg) asprintf(&p, "%s/betl/dotnet", xdg);
    else if (home)   asprintf(&p, "%s/.cache/betl/dotnet", home);
    else             p = strdup("/tmp/betl-cache/dotnet");
    return p;
}

/* mkdir -p */
static int mkdir_p(const char *path) {
    char *copy = strdup(path);
    if (!copy) return -1;
    for (char *p = copy + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
                free(copy); return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
        free(copy); return -1;
    }
    free(copy);
    return 0;
}


/* ============================================================== *
 *  Hash for cache key                                              *
 *                                                                  *
 *  FNV-1a 64-bit, hex-encoded. The cache key only protects against *
 *  accidental collisions during dev; a malicious crafted source     *
 *  isn't a threat model here because the user controls their own    *
 *  pipeline source. If we ever want stronger guarantees, swap in    *
 *  SHA-256.                                                         *
 * ============================================================== */

static void fnv1a_64(const char *data, size_t n, uint64_t *state) {
    uint64_t h = *state;
    for (size_t i = 0; i < n; ++i) {
        h ^= (uint8_t)data[i];
        h *= 0x100000001b3ULL;
    }
    *state = h;
}

/* Hash every file in the shim dir, sorted by filename, into the
 * cache key seed. Without this a developer-side shim edit silently
 * hits the old cache and dlopens a stale .so — observed during phase
 * 1 development. */
static int hash_shim_dir(const char *shim_dir, uint64_t *h) {
    DIR *d = opendir(shim_dir);
    if (!d) return -1;
    /* Collect filenames, sort for determinism. */
    char *names[64] = {0};
    size_t n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < sizeof names / sizeof names[0]) {
        if (e->d_name[0] == '.') continue;
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    /* Simple insertion sort — n is tiny. */
    for (size_t i = 1; i < n; ++i) {
        for (size_t j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; --j) {
            char *tmp = names[j]; names[j] = names[j - 1]; names[j - 1] = tmp;
        }
    }
    for (size_t i = 0; i < n; ++i) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", shim_dir, names[i]);
        FILE *f = fopen(path, "rb");
        if (!f) { for (size_t k = 0; k < n; ++k) free(names[k]); return -1; }
        fnv1a_64(names[i], strlen(names[i]), h);
        fnv1a_64("|", 1, h);
        char buf[4096];
        size_t r;
        while ((r = fread(buf, 1, sizeof buf, f)) > 0) {
            fnv1a_64(buf, r, h);
        }
        fclose(f);
        fnv1a_64("\0", 1, h);
        free(names[i]);
    }
    return 0;
}

static int compute_cache_key(const char *source, const char *lang,
                             const char *kind, const char *shim_dir,
                             char out[17]) {
    uint64_t h = 0xcbf29ce484222325ULL;
    fnv1a_64(BETL_DOTNET_SHIM_ABI_VERSION, strlen(BETL_DOTNET_SHIM_ABI_VERSION), &h);
    fnv1a_64("|", 1, &h);
    fnv1a_64(kind, strlen(kind), &h);
    fnv1a_64("|", 1, &h);
    fnv1a_64(lang, strlen(lang), &h);
    fnv1a_64("|", 1, &h);
    fnv1a_64(source, strlen(source), &h);
    fnv1a_64("|", 1, &h);
    if (hash_shim_dir(shim_dir, &h) != 0) return -1;
    snprintf(out, 17, "%016" PRIx64, h);
    return 0;
}


/* ============================================================== *
 *  fork/exec wrapper for `dotnet publish`                          *
 * ============================================================== */

/* Spawn `argv` in `cwd` with optional extra envs (KEY=VAL strings,
 * NULL-terminated). The child's combined stdout+stderr is written to
 * `log_path` so the full output survives even if it's many MB of
 * NativeAOT linker chatter — `err`/`err_cap` get a short tail for
 * the error message itself. Returns child's exit status or -1 on
 * fork/pipe failure. */
static int run_command(const char *cwd, char *const argv[],
                       char *const extra_envs[],
                       const char *log_path,
                       char *err, size_t err_cap) {
    int pipefd[2] = {-1, -1};
    if (pipe(pipefd) != 0) {
        snprintf(err, err_cap, "pipe: %s", strerror(errno));
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        snprintf(err, err_cap, "fork: %s", strerror(errno));
        close(pipefd[0]); close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(127);
        if (dup2(pipefd[1], STDERR_FILENO) < 0) _exit(127);
        close(pipefd[1]);
        if (cwd && chdir(cwd) != 0) _exit(127);
        for (int i = 0; extra_envs && extra_envs[i]; ++i) {
            putenv(extra_envs[i]);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pipefd[1]);

    int log_fd = log_path ? open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644) : -1;
    /* Keep a small rolling tail for the short error message. */
    char tail[2048] = {0};
    size_t tail_n = 0;
    char buf[4096];
    for (;;) {
        ssize_t n = read(pipefd[0], buf, sizeof buf);
        if (n <= 0) break;
        if (log_fd >= 0) {
            ssize_t written = write(log_fd, buf, (size_t)n);
            (void)written;
        }
        /* Tail buffer: keep the most recent ~2KB. */
        if ((size_t)n >= sizeof tail) {
            memcpy(tail, buf + (size_t)n - sizeof tail + 1, sizeof tail - 1);
            tail[sizeof tail - 1] = '\0';
            tail_n = sizeof tail - 1;
        } else if (tail_n + (size_t)n < sizeof tail) {
            memcpy(tail + tail_n, buf, (size_t)n);
            tail_n += (size_t)n;
            tail[tail_n] = '\0';
        } else {
            size_t drop = tail_n + (size_t)n - (sizeof tail - 1);
            memmove(tail, tail + drop, tail_n - drop);
            tail_n -= drop;
            memcpy(tail + tail_n, buf, (size_t)n);
            tail_n += (size_t)n;
            tail[tail_n] = '\0';
        }
    }
    close(pipefd[0]);
    if (log_fd >= 0) close(log_fd);

    int status = 0;
    waitpid(pid, &status, 0);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (exit_code != 0) {
        snprintf(err, err_cap, "tail of output:\n%s", tail);
    }
    return exit_code;
}


/* ============================================================== *
 *  Compile cache                                                   *
 * ============================================================== */

/* Copy `src_path` → `dst_path` byte-for-byte. */
static int copy_file(const char *src_path, const char *dst_path) {
    int sfd = open(src_path, O_RDONLY);
    if (sfd < 0) return -1;
    int dfd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dfd < 0) { close(sfd); return -1; }
    char buf[8192];
    ssize_t n;
    while ((n = read(sfd, buf, sizeof buf)) > 0) {
        if (write(dfd, buf, (size_t)n) != n) {
            close(sfd); close(dfd); unlink(dst_path); return -1;
        }
    }
    close(sfd); close(dfd);
    return n < 0 ? -1 : 0;
}

/* Copy every file (non-recursive) from src_dir to dst_dir. */
static int copy_dir_flat(const char *src_dir, const char *dst_dir) {
    DIR *d = opendir(src_dir);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char *sp = NULL, *dp = NULL;
        asprintf(&sp, "%s/%s", src_dir, e->d_name);
        asprintf(&dp, "%s/%s", dst_dir, e->d_name);
        if (!sp || !dp) { free(sp); free(dp); closedir(d); return -1; }
        int rc = copy_file(sp, dp);
        free(sp); free(dp);
        if (rc != 0) { closedir(d); return -1; }
    }
    closedir(d);
    return 0;
}

/* Compile `source` (lang = "csharp" or "vbnet", kind = "task" or
 * "script") into a cached .so. On success *out_path holds the cache
 * path (caller frees). */
static int compile_to_cache(BetlContext *ctx,
                            const char *source, const char *lang,
                            const char *kind,
                            char **out_path,
                            char *err, size_t err_cap) {
    char *cache = cache_dir();
    if (!cache || mkdir_p(cache) != 0) {
        snprintf(err, err_cap, "cache_dir: %s", strerror(errno));
        free(cache); return -1;
    }

    /* Shim dir is needed both for the cache key (hash shim contents)
     * and for the build materialisation below — locate it first. */
    char *shim_dir = find_shim_dir();
    if (!shim_dir) {
        snprintf(err, err_cap,
            "betl-dotnet shim dir not found (set BETL_DOTNET_SHIM_DIR)");
        free(cache); return -1;
    }

    char key[17];
    if (compute_cache_key(source, lang, kind, shim_dir, key) != 0) {
        snprintf(err, err_cap, "cache key hash failed (shim_dir unreadable?)");
        free(cache); free(shim_dir); return -1;
    }

    char *cached = NULL;
    asprintf(&cached, "%s/%s.so", cache, key);
    if (!cached) { free(cache); free(shim_dir); return -1; }

    if (file_exists(cached)) {
        free(cache); free(shim_dir);
        *out_path = cached;
        return 0;
    }

    char *dotnet = find_dotnet();
    if (!dotnet) {
        snprintf(err, err_cap,
            "dotnet binary not found (set BETL_DOTNET_ROOT or run "
            "scripts/install-dotnet.sh)");
        free(cache); free(cached); free(shim_dir); return -1;
    }

    /* Materialise build dir under the cache (so an interrupted compile
     * leaves a clear trail under XDG_CACHE_HOME for inspection). */
    char build_dir[256];
    snprintf(build_dir, sizeof build_dir, "%s/build-%s", cache, key);
    if (mkdir_p(build_dir) != 0) {
        snprintf(err, err_cap, "mkdir build_dir: %s", strerror(errno));
        goto fail;
    }
    if (copy_dir_flat(shim_dir, build_dir) != 0) {
        snprintf(err, err_cap, "copy shim files");
        goto fail;
    }
    /* Write user source. C# only for phase 1; VB.NET wires in later. */
    char *src_path = NULL;
    asprintf(&src_path, "%s/UserScript.cs", build_dir);
    if (!src_path) goto fail;
    int sfd = open(src_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (sfd < 0) {
        snprintf(err, err_cap, "open %s: %s", src_path, strerror(errno));
        free(src_path); goto fail;
    }
    size_t slen = strlen(source);
    if (write(sfd, source, slen) != (ssize_t)slen) {
        close(sfd); free(src_path);
        snprintf(err, err_cap, "write UserScript.cs");
        goto fail;
    }
    close(sfd);
    free(src_path);

    betl_log(ctx, BETL_LOG_INFO,
        "dotnet.task: AOT-compiling user script (cache key %s)", key);

    /* Run `dotnet publish`. Output goes to build_dir/out/. */
    char *out_arg = NULL;
    asprintf(&out_arg, "%s/out", build_dir);
    char *argv[] = {
        dotnet,
        "publish",
        "-c", "Release",
        "-r", "linux-x64",
        "-o", out_arg,
        "--nologo",
        NULL
    };
    /* InvariantGlobalization avoids a libicu runtime dep in the AOT'd
     * output — the shim doesn't need locale-aware string handling.
     * DOTNET_CLI_TELEMETRY_OPTOUT keeps logs clean. */
    static char env1[] = "DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1";
    static char env2[] = "DOTNET_CLI_TELEMETRY_OPTOUT=1";
    static char env3[] = "DOTNET_NOLOGO=1";
    char *dotnet_root_env = NULL;
    {
        /* Some SDKs need DOTNET_ROOT set if invoked outside a shell
         * that already exported it. */
        char *dotnet_dir = strdup(dotnet);
        if (dotnet_dir) {
            char *slash = strrchr(dotnet_dir, '/');
            if (slash) *slash = '\0';
            asprintf(&dotnet_root_env, "DOTNET_ROOT=%s", dotnet_dir);
            free(dotnet_dir);
        }
    }
    char *envs[] = { env1, env2, env3, dotnet_root_env, NULL };
    char log_path[512];
    snprintf(log_path, sizeof log_path, "%s/publish.log", build_dir);
    int rc = run_command(build_dir, argv, envs, log_path, err, err_cap);
    free(out_arg);
    free(dotnet_root_env);
    if (rc != 0) {
        /* Prepend the full-log path to the tail snippet so the user
         * can see the complete failure even when it's MB of output. */
        char full_err[4096];
        snprintf(full_err, sizeof full_err,
            "dotnet publish exited %d. Full log: %s\n%s",
            rc, log_path, err);
        snprintf(err, err_cap, "%s", full_err);
        goto fail;
    }

    /* Find the produced .so (Betl.Shim.so). */
    char *produced = NULL;
    asprintf(&produced, "%s/out/Betl.Shim.so", build_dir);
    if (!file_exists(produced)) {
        snprintf(err, err_cap, "produced .so missing at %s", produced);
        free(produced); goto fail;
    }
    if (rename(produced, cached) != 0) {
        /* Cross-device? Fall back to copy. */
        if (copy_file(produced, cached) != 0) {
            snprintf(err, err_cap, "move %s → %s: %s",
                     produced, cached, strerror(errno));
            free(produced); goto fail;
        }
    }
    free(produced);
    free(cache); free(dotnet); free(shim_dir);
    *out_path = cached;
    return 0;

fail:
    free(cache); free(cached); free(dotnet); free(shim_dir);
    return -1;
}


/* ============================================================== *
 *  Host bridge function pointers (passed to C# at init)            *
 * ============================================================== */

/* Wrappers so we feed C# plain (ctx, level, c-string) without exposing
 * betl_log's printf-style variadic surface to the function-pointer
 * marshaller on the C# side. */
static void host_log_wrapper(BetlContext *ctx, int level, const char *msg) {
    betl_log(ctx, (BetlLogLevel)level, "%s", msg ? msg : "");
}

static const char *host_get_param_wrapper(BetlContext *ctx, const char *name) {
    return betl_get_param(ctx, name);
}


/* ============================================================== *
 *  dotnet.task                                                     *
 * ============================================================== */

typedef int  (*dotnet_init_fn)(BetlContext *ctx,
                               void (*log)(BetlContext *, int, const char *),
                               const char *(*get_param)(BetlContext *, const char *));
typedef int  (*dotnet_task_run_fn)(void);

typedef struct {
    BetlContext       *ctx;
    char              *source;
    char              *lang;
    char              *so_path;       /* cache path */
    void              *handle;        /* dlopen */
    dotnet_init_fn     init_fn;
    dotnet_task_run_fn run_fn;
    char               err[8192];
} DotnetTask;

static int dotnet_task_init(BetlContext *ctx, const char *cfg, void **state) {
    DotnetTask *t = calloc(1, sizeof *t);
    if (!t) return BETL_ERR_INTERNAL;
    t->ctx = ctx;

    if (json_get_string(cfg, "source", &t->source) != 0 || !t->source) {
        betl_set_error(ctx, "dotnet.task: 'source' is required");
        free(t); return BETL_ERR_INVALID;
    }
    if (json_get_string(cfg, "lang", &t->lang) != 0 || !t->lang) {
        /* Default to C#. */
        t->lang = strdup("csharp");
    }
    if (strcmp(t->lang, "csharp") != 0 && strcmp(t->lang, "vbnet") != 0) {
        betl_set_error(ctx, "dotnet.task: 'lang' must be 'csharp' or 'vbnet' (got '%s')",
                       t->lang);
        free(t->source); free(t->lang); free(t);
        return BETL_ERR_INVALID;
    }
    if (strcmp(t->lang, "vbnet") == 0) {
        betl_set_error(ctx, "dotnet.task: lang=vbnet not yet implemented "
                            "(v0.2 phase 2)");
        free(t->source); free(t->lang); free(t);
        return BETL_ERR_UNSUPPORTED;
    }

    if (compile_to_cache(ctx, t->source, t->lang, "task",
                         &t->so_path, t->err, sizeof t->err) != 0) {
        betl_set_error(ctx, "dotnet.task: compile failed: %s", t->err);
        free(t->source); free(t->lang); free(t);
        return BETL_ERR_INVALID;
    }

    t->handle = dlopen(t->so_path, RTLD_NOW | RTLD_LOCAL);
    if (!t->handle) {
        betl_set_error(ctx, "dotnet.task: dlopen(%s) failed: %s",
                       t->so_path, dlerror());
        free(t->so_path); free(t->source); free(t->lang); free(t);
        return BETL_ERR_INVALID;
    }
    /* Object-pointer ↔ function-pointer dance per POSIX dlsym
     * commentary — bytes are identical, the C standard just hates
     * the direct cast. */
    *(void **)&t->init_fn = dlsym(t->handle, "betl_dotnet_init");
    *(void **)&t->run_fn  = dlsym(t->handle, "betl_dotnet_task_run");
    if (!t->init_fn || !t->run_fn) {
        betl_set_error(ctx, "dotnet.task: missing entry points in %s "
                            "(rebuild after shim ABI change?)", t->so_path);
        /* Do not dlclose — see dotnet_task_destroy for the rationale. */
        free(t->so_path); free(t->source); free(t->lang); free(t);
        return BETL_ERR_INVALID;
    }

    if (t->init_fn(ctx, host_log_wrapper, host_get_param_wrapper) != 0) {
        betl_set_error(ctx, "dotnet.task: managed init returned non-zero");
        /* Do not dlclose — see dotnet_task_destroy. */
        free(t->so_path); free(t->source); free(t->lang); free(t);
        return BETL_ERR_INVALID;
    }

    *state = t;
    return BETL_OK;
}

static void dotnet_task_destroy(void *state) {
    if (!state) return;
    DotnetTask *t = state;
    /* Deliberately do NOT dlclose(t->handle): NativeAOT-compiled
     * assemblies install a finalizer thread + GC threads at first
     * managed call, and tearing them down on process-still-alive
     * dlclose has been observed to SEGV. The handle survives until
     * process exit, when the loader unmaps the .so as part of normal
     * teardown and managed threads exit together. This is the same
     * trade-off CoreCLR's own host APIs make. */
    free(t->so_path); free(t->source); free(t->lang); free(t);
}

static int dotnet_task_run(void *state) {
    DotnetTask *t = state;
    int rc = t->run_fn();
    return rc == 0 ? BETL_OK : BETL_ERR_INTERNAL;
}


/* ============================================================== *
 *  Provider entry                                                  *
 * ============================================================== */

static const BetlComponentDef components[] = {
    { .name               = "dotnet.task",
      .kind               = BETL_KIND_TASK,
      .config_schema_json = "{\"type\":\"object\","
                             "\"properties\":{"
                               "\"source\":{\"type\":\"string\"},"
                               "\"lang\":{\"type\":\"string\",\"enum\":[\"csharp\",\"vbnet\"]}"
                             "},"
                             "\"required\":[\"source\"]}",
      .flags              = 0,
      .init               = dotnet_task_init,
      .destroy            = dotnet_task_destroy,
      .task_run           = dotnet_task_run },
};

static const BetlProvider dotnet_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-dotnet",
    .version         = "0.2.0-dev",
    .license         = "MIT",
    .components      = components,
    .component_count = sizeof components / sizeof components[0],
};

BETL_EXPORT const BetlProvider *betl_provider_entry(void) {
    return &dotnet_provider;
}
