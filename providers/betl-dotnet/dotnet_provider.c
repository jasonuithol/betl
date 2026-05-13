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
#define BETL_DOTNET_SHIM_ABI_VERSION "3"


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

/* Hash every file in the shim dir (recursing one level into kind-
 * specific subdirs), sorted by filename, into the cache key seed.
 * Without this a developer-side shim edit silently hits the old
 * cache and dlopens a stale .so — observed during phase 1
 * development. */
static int hash_one_dir(const char *dir, const char *prefix, uint64_t *h) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    char *names[64] = {0};
    size_t n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < sizeof names / sizeof names[0]) {
        if (e->d_name[0] == '.') continue;
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    for (size_t i = 1; i < n; ++i) {
        for (size_t j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; --j) {
            char *tmp = names[j]; names[j] = names[j - 1]; names[j - 1] = tmp;
        }
    }
    for (size_t i = 0; i < n; ++i) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        struct stat st;
        if (stat(path, &st) != 0) {
            for (size_t k = i; k < n; ++k) free(names[k]);
            return -1;
        }
        if (S_ISDIR(st.st_mode)) {
            char sub_prefix[256];
            snprintf(sub_prefix, sizeof sub_prefix, "%s%s/", prefix, names[i]);
            int rc = hash_one_dir(path, sub_prefix, h);
            free(names[i]);
            if (rc != 0) { for (size_t k = i + 1; k < n; ++k) free(names[k]); return -1; }
            continue;
        }
        FILE *f = fopen(path, "rb");
        if (!f) { for (size_t k = i; k < n; ++k) free(names[k]); return -1; }
        fnv1a_64(prefix, strlen(prefix), h);
        fnv1a_64(names[i], strlen(names[i]), h);
        fnv1a_64("|", 1, h);
        char buf[4096];
        size_t r;
        while ((r = fread(buf, 1, sizeof buf, f)) > 0) fnv1a_64(buf, r, h);
        fclose(f);
        fnv1a_64("\0", 1, h);
        free(names[i]);
    }
    return 0;
}

static int hash_shim_dir(const char *shim_dir, uint64_t *h) {
    return hash_one_dir(shim_dir, "", h);
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
        /* Skip subdirectories — callers select which subdirs to merge
         * in via the compile_request_t.shim_subdir field. */
        struct stat st;
        if (stat(sp, &st) == 0 && S_ISDIR(st.st_mode)) {
            free(sp); free(dp); continue;
        }
        int rc = copy_file(sp, dp);
        free(sp); free(dp);
        if (rc != 0) { closedir(d); return -1; }
    }
    closedir(d);
    return 0;
}

/* Extra source files the caller wants in the build dir on top of the
 * shim. Used by dotnet.script to inject a Generated.cs containing
 * schema-typed InputRow / OutputRow definitions + per-cell extraction
 * and emit code. */
typedef struct {
    const char *filename;     /* basename only — written into build_dir */
    const char *content;
    size_t      content_len;
} extra_file_t;

typedef struct {
    const char         *user_source;
    const char         *lang;          /* "csharp" only for now */
    const char         *kind;          /* "task" or "script" */
    const extra_file_t *extra_files;   /* NULL when n_extra is 0 */
    size_t              n_extra;
    /* Extra shim subdir to merge into the build (e.g. "script" for
     * dotnet.script). The plugin's shim/ ships task-shared files at
     * the top level and kind-specific files under shim/<kind>/. */
    const char         *shim_subdir;   /* NULL or e.g. "script" */
    /* Additional bytes mixed into the cache key. Used by dotnet.script
     * to invalidate the cache when the input/output schema changes. */
    const char         *extra_hash_in;
    size_t              extra_hash_len;
} compile_request_t;

/* Compile per `req` into a cached .so. On success *out_path holds the
 * cache path (caller frees). */
static int compile_to_cache(BetlContext *ctx,
                            const compile_request_t *req,
                            char **out_path,
                            char *err, size_t err_cap) {
    const char *source = req->user_source;
    const char *lang   = req->lang;
    const char *kind   = req->kind;
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
    /* Re-hash the user source + extra inputs back into the key when
     * extra_hash_in is set. We do this by recomputing the hex string
     * with extra bytes appended via fnv1a chaining on the existing
     * value — simpler than re-engineering compute_cache_key. */
    if (req->extra_hash_in && req->extra_hash_len) {
        uint64_t h;
        sscanf(key, "%" SCNx64, &h);
        fnv1a_64("|", 1, &h);
        fnv1a_64(req->extra_hash_in, req->extra_hash_len, &h);
        snprintf(key, sizeof key, "%016" PRIx64, h);
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
    if (req->shim_subdir) {
        char *sub_src = NULL, *sub_dst = NULL;
        asprintf(&sub_src, "%s/%s", shim_dir, req->shim_subdir);
        asprintf(&sub_dst, "%s/%s", build_dir, req->shim_subdir);
        if (!sub_src || !sub_dst) {
            free(sub_src); free(sub_dst);
            snprintf(err, err_cap, "OOM building subdir paths");
            goto fail;
        }
        if (mkdir_p(sub_dst) != 0 || copy_dir_flat(sub_src, sub_dst) != 0) {
            free(sub_src); free(sub_dst);
            snprintf(err, err_cap, "copy shim/%s files", req->shim_subdir);
            goto fail;
        }
        free(sub_src); free(sub_dst);
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

    /* Drop any extra files the caller wants (Generated.cs for
     * dotnet.script). Same dir, same .csproj globbing — they get
     * compiled together. */
    for (size_t i = 0; i < req->n_extra; ++i) {
        char *ep = NULL;
        asprintf(&ep, "%s/%s", build_dir, req->extra_files[i].filename);
        if (!ep) { snprintf(err, err_cap, "OOM"); goto fail; }
        int efd = open(ep, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (efd < 0) {
            snprintf(err, err_cap, "open %s: %s", ep, strerror(errno));
            free(ep); goto fail;
        }
        if (write(efd, req->extra_files[i].content,
                  req->extra_files[i].content_len)
            != (ssize_t)req->extra_files[i].content_len) {
            close(efd); free(ep);
            snprintf(err, err_cap, "write %s", req->extra_files[i].filename);
            goto fail;
        }
        close(efd); free(ep);
    }

    betl_log(ctx, BETL_LOG_INFO,
        "dotnet.%s: AOT-compiling user script (cache key %s)", kind, key);

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

static const char *host_get_connection_wrapper(BetlContext *ctx, const char *name) {
    return betl_get_connection(ctx, name);
}


/* ============================================================== *
 *  dotnet.task                                                     *
 * ============================================================== */

typedef int  (*dotnet_init_fn)(BetlContext *ctx,
                               void (*log)(BetlContext *, int, const char *),
                               const char *(*get_param)(BetlContext *, const char *),
                               const char *(*get_connection)(BetlContext *, const char *));
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
        /* Hard architectural call: VB.NET Roslyn explicitly rejects
         * [UnmanagedCallersOnly] (error BC37316), so a VB.NET source
         * can't host our entry points directly. Rather than emulate
         * with a C# shim + project-reference dance (which fights the
         * NativeAOT trim+reflection model), betl translates VB.NET to
         * C# at DTSX-conversion time and the runtime sees only C#.
         *
         * If you have hand-written VB.NET pipeline source, run it
         * through the DTSX converter's --translate-vb flag (or any
         * Roslyn-based VB→C# tool) and submit the C# output. */
        betl_set_error(ctx,
            "dotnet.task: lang=vbnet is not a runtime language; "
            "translate to C# via the DTSX converter (or any VB→C# tool) "
            "and submit lang=csharp");
        free(t->source); free(t->lang); free(t);
        return BETL_ERR_UNSUPPORTED;
    }

    compile_request_t creq = {
        .user_source = t->source,
        .lang        = t->lang,
        .kind        = "task",
        .shim_subdir = "task",
    };
    if (compile_to_cache(ctx, &creq, &t->so_path, t->err, sizeof t->err) != 0) {
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

    if (t->init_fn(ctx, host_log_wrapper, host_get_param_wrapper,
                   host_get_connection_wrapper) != 0) {
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
 *  dotnet.script — async transform                                 *
 *                                                                  *
 *  Compile model differs from dotnet.task in two ways:              *
 *    - Schema-typed Row classes generated from input + output_schema*
 *      at init time, alongside the user source.                     *
 *    - Compile happens lazily at first get_next (input schema isn't *
 *      known until then), not at component init.                    *
 * ============================================================== */

typedef enum {
    DS_INT64 = 1,
    DS_FLOAT64,
    DS_BOOL,
    DS_UTF8,
    /* Phase 1b narrow numerics. Storage piggy-backs on i64_vals /
     * f64_vals — the values are widened on input and narrowed at
     * finalize, so the staging hot path stays one-type-per-vector. */
    DS_INT8,
    DS_INT16,
    DS_INT32,
    DS_UINT8,
    DS_UINT16,
    DS_UINT32,
    DS_UINT64,
    DS_FLOAT32,
} DsType;

typedef struct {
    char  *name;
    DsType type;
    char   arrow_fmt;     /* 'l' / 'g' / 'b' / 'u' / 'c' / 'C' / 's' / 'S' / 'i' / 'I' / 'L' / 'f' */
} DsCol;

/* Map an Arrow format char to a DsType, or DS_INT64 + return -1 on
 * unsupported. Single source of truth for the supported set. */
static int ds_fmt_to_type(char fmt, DsType *out) {
    switch (fmt) {
        case 'l': *out = DS_INT64;   return 0;
        case 'g': *out = DS_FLOAT64; return 0;
        case 'b': *out = DS_BOOL;    return 0;
        case 'u': *out = DS_UTF8;    return 0;
        case 'c': *out = DS_INT8;    return 0;
        case 's': *out = DS_INT16;   return 0;
        case 'i': *out = DS_INT32;   return 0;
        case 'C': *out = DS_UINT8;   return 0;
        case 'S': *out = DS_UINT16;  return 0;
        case 'I': *out = DS_UINT32;  return 0;
        case 'L': *out = DS_UINT64;  return 0;
        case 'f': *out = DS_FLOAT32; return 0;
        default:  *out = DS_INT64;   return -1;
    }
}

static char ds_type_to_fmt(DsType t) {
    switch (t) {
        case DS_INT64:   return 'l';
        case DS_FLOAT64: return 'g';
        case DS_BOOL:    return 'b';
        case DS_UTF8:    return 'u';
        case DS_INT8:    return 'c';
        case DS_INT16:   return 's';
        case DS_INT32:   return 'i';
        case DS_UINT8:   return 'C';
        case DS_UINT16:  return 'S';
        case DS_UINT32:  return 'I';
        case DS_UINT64:  return 'L';
        case DS_FLOAT32: return 'f';
    }
    return '?';
}

/* Group test: does this type use the i64_vals storage vector? */
static int ds_type_is_int64_stored(DsType t) {
    return t == DS_INT64 || t == DS_INT8 || t == DS_INT16 || t == DS_INT32
        || t == DS_UINT8 || t == DS_UINT16 || t == DS_UINT32 || t == DS_UINT64;
}
/* Does this type use the f64_vals storage vector? */
static int ds_type_is_f64_stored(DsType t) {
    return t == DS_FLOAT64 || t == DS_FLOAT32;
}

/* Per-output-column growable staging — populated by Emit setter
 * callbacks from C#, flushed to an Arrow leaf at end of each batch. */
typedef struct {
    DsType   type;
    size_t   cap;
    size_t   n;             /* set rows (= committed row count) */
    uint8_t *nulls;
    int64_t *i64_vals;
    double  *f64_vals;
    uint8_t *b_vals;        /* packed at finalize */
    int32_t *u8_offsets;
    char    *u8_data;
    size_t   u8_len;
    size_t   u8_cap;
    /* The cell-set flag tracks which cells of the in-flight row have
     * been written this commit cycle. Used to fall back to NULL for
     * any column the script forgot to set. */
    uint8_t  pending_null[1];   /* placeholder; really one bool per col */
} DsOutCol;

/* Shared emit context — what the C# setters receive back via the
 * opaque emit_ctx pointer. Stack-built in each get_next loop so it
 * captures the current out_staging / pending_set pointers + n_out.
 * Letting both dotnet.script and dotnet.pipelinecomponent route
 * through the same setters; the per-kind structs hold the same
 * fields as top-level members and the emitter is just a shim. */
typedef struct {
    DsOutCol *out_staging;
    int      *pending_set;
    size_t    n_out;
} DsEmitter;

typedef int (*ds_init_fn)(BetlContext *ctx,
                          void (*log)(BetlContext *, int, const char *),
                          const char *(*get_param)(BetlContext *, const char *),
                          const char *(*get_connection)(BetlContext *, const char *));
typedef int (*ds_script_init_fn)(void);
typedef int (*ds_register_emit_fn)(
    void (*set_int64)  (void *, int, int64_t),
    void (*set_float64)(void *, int, double),
    void (*set_bool)   (void *, int, uint8_t),
    void (*set_utf8)   (void *, int, const uint8_t *, int),
    void (*set_null)   (void *, int),
    void (*commit_row) (void *));
typedef int (*ds_process_batch_fn)(struct ArrowArray *batch, void *emit_ctx);
typedef int (*ds_on_eof_fn)(void *emit_ctx);

typedef struct {
    BetlContext             *ctx;
    char                    *source;
    char                    *lang;
    /* output_schema parsed from YAML */
    DsCol                   *out_cols;
    size_t                   n_out;
    /* input_schema cached from upstream on first get_next */
    DsCol                   *in_cols;
    size_t                   n_in;

    /* dlopen / function pointers */
    char                    *so_path;
    void                    *handle;
    ds_init_fn               init_fn;
    ds_script_init_fn        script_init_fn;
    ds_register_emit_fn      register_emit_fn;
    ds_process_batch_fn      process_batch_fn;
    ds_on_eof_fn             on_eof_fn;

    struct ArrowArrayStream  input;
    int                      have_input;
    int                      compiled;
    int                      eof_seen;

    DsOutCol                *out_staging;
    int                     *pending_set;   /* per-col flag, reset on commit */
    char                     last_err[1024];
} DotnetScript;

static void ds_set_err(DotnetScript *s, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(s->last_err, sizeof s->last_err, fmt, ap); va_end(ap);
    betl_set_error(s->ctx, "%s", s->last_err);
}

/* --- YAML output_schema parsing ------------------------------- */

/* Minimal array walker for output_schema. JSON shape:
 *   [{"name":"...","type":"l"},{"name":"...","type":"g"},...] */
static int parse_output_schema(DotnetScript *s, const char *cfg) {
    const char *p = json_value_after(cfg, "output_schema");
    if (!p || *p != '[') {
        ds_set_err(s, "dotnet.script: 'output_schema' is required (array)");
        return -1;
    }
    ++p;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    if (*p == ']') { ds_set_err(s, "dotnet.script: 'output_schema' is empty"); return -1; }
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
        if (*p != '{') { ds_set_err(s, "dotnet.script: output_schema entry must be an object"); return -1; }
        /* find the matching } — track depth + skip strings */
        const char *start = p;
        int depth = 0;
        for (; *p; ++p) {
            if (*p == '"') {
                ++p;
                while (*p && *p != '"') { if (*p == '\\' && p[1]) ++p; ++p; }
                if (!*p) { ds_set_err(s, "dotnet.script: malformed output_schema"); return -1; }
                continue;
            }
            if (*p == '{') ++depth;
            else if (*p == '}') { --depth; if (depth == 0) { ++p; break; } }
        }
        size_t len = (size_t)(p - start);
        char *buf = malloc(len + 1);
        if (!buf) return -1;
        memcpy(buf, start, len); buf[len] = '\0';

        char *name = NULL, *type = NULL;
        json_get_string(buf, "name", &name);
        json_get_string(buf, "type", &type);
        free(buf);
        if (!name || !type) {
            ds_set_err(s, "dotnet.script: output column needs name + type");
            free(name); free(type); return -1;
        }
        DsType t;
        if (strlen(type) != 1 || ds_fmt_to_type(type[0], &t) != 0) {
            ds_set_err(s, "dotnet.script: output column '%s' type '%s' "
                          "not supported (Phase 1b: l/g/b/u + c/C/s/S/i/I/L/f)",
                       name, type);
            free(name); free(type); return -1;
        }
        free(type);
        DsCol *grow = realloc(s->out_cols, (s->n_out + 1) * sizeof *grow);
        if (!grow) { free(name); return -1; }
        s->out_cols = grow;
        s->out_cols[s->n_out].name = name;
        s->out_cols[s->n_out].type = t;
        s->out_cols[s->n_out].arrow_fmt = ds_type_to_fmt(t);
        ++s->n_out;

        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
        if (*p == ',') { ++p; continue; }
        if (*p == ']') return 0;
        ds_set_err(s, "dotnet.script: malformed output_schema array");
        return -1;
    }
}

/* --- input schema caching --------------------------------------- */

static int cache_input_schema(DotnetScript *s) {
    if (s->in_cols) return 0;
    struct ArrowSchema up = {0};
    if (s->input.get_schema(&s->input, &up) != 0) {
        ds_set_err(s, "dotnet.script: upstream get_schema failed");
        return -1;
    }
    int rc = -1;
    if (!up.format || strcmp(up.format, "+s") != 0 || up.n_children <= 0) {
        ds_set_err(s, "dotnet.script: input must be a struct"); goto done;
    }
    size_t n = (size_t)up.n_children;
    DsCol *cols = calloc(n, sizeof *cols);
    if (!cols) { ds_set_err(s, "dotnet.script: OOM"); goto done; }
    for (size_t i = 0; i < n; ++i) {
        struct ArrowSchema *c = up.children[i];
        const char *fmt = c && c->format ? c->format : "";
        DsType t;
        if (strlen(fmt) != 1 || ds_fmt_to_type(fmt[0], &t) != 0) {
            ds_set_err(s, "dotnet.script: input column '%s' has unsupported "
                          "Arrow format '%s' (Phase 1b: l/g/b/u + c/C/s/S/i/I/L/f)",
                       c && c->name ? c->name : "?", fmt);
            for (size_t k = 0; k < i; ++k) free(cols[k].name);
            free(cols); goto done;
        }
        cols[i].name = strdup(c && c->name ? c->name : "");
        cols[i].type = t;
        cols[i].arrow_fmt = fmt[0];
        if (!cols[i].name) {
            for (size_t k = 0; k < i; ++k) free(cols[k].name);
            free(cols); goto done;
        }
    }
    s->in_cols = cols;
    s->n_in = n;
    rc = 0;
done:
    if (up.release) up.release(&up);
    return rc;
}

/* --- Generated.cs codegen -------------------------------------- */

/* A growable string buffer for assembling generated C# source. */
typedef struct { char *p; size_t len, cap; } Sbuf;
static int sb_append(Sbuf *s, const char *str) {
    size_t n = strlen(str);
    if (s->len + n + 1 > s->cap) {
        size_t nc = s->cap ? s->cap : 1024;
        while (nc < s->len + n + 1) nc *= 2;
        char *p = realloc(s->p, nc);
        if (!p) return -1;
        s->p = p; s->cap = nc;
    }
    memcpy(s->p + s->len, str, n);
    s->len += n;
    s->p[s->len] = '\0';
    return 0;
}
static int sb_appendf(Sbuf *s, const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof buf) return -1;
    return sb_append(s, buf);
}

/* C# property type for a DsType (already nullable for value types,
 * naturally nullable for reference types). */
static const char *ds_cs_type(DsType t) {
    switch (t) {
        case DS_INT64:   return "long?";
        case DS_FLOAT64: return "double?";
        case DS_BOOL:    return "bool?";
        case DS_UTF8:    return "string?";
        case DS_INT8:    return "sbyte?";
        case DS_INT16:   return "short?";
        case DS_INT32:   return "int?";
        case DS_UINT8:   return "byte?";
        case DS_UINT16:  return "ushort?";
        case DS_UINT32:  return "uint?";
        case DS_UINT64:  return "ulong?";
        case DS_FLOAT32: return "float?";
    }
    return "object?";
}

/* Native C# unmanaged-pointer type for the Arrow values buffer of a
 * given DsType — used in generated input-extraction code. */
static const char *ds_cs_buffer_ptr_type(DsType t) {
    switch (t) {
        case DS_INT64:   return "long";
        case DS_FLOAT64: return "double";
        case DS_INT8:    return "sbyte";
        case DS_INT16:   return "short";
        case DS_INT32:   return "int";
        case DS_UINT8:   return "byte";
        case DS_UINT16:  return "ushort";
        case DS_UINT32:  return "uint";
        case DS_UINT64:  return "ulong";
        case DS_FLOAT32: return "float";
        default:         return "byte"; /* unused */
    }
}

/* Emit the per-column extraction for one input cell into a generated
 * if-null/else-read pattern. */
static int gen_input_extract(Sbuf *out, const DsCol *c, size_t idx) {
    sb_appendf(out,
        "    {\n"
        "      var c = batch->Children[%zu];\n"
        "      long rowOff = c->Offset + row;\n"
        "      bool isNull = c->NullCount > 0 && c->Buffers[0] != null && "
        "        ((((byte*)c->Buffers[0])[rowOff/8] >> (int)(rowOff&7)) & 1u) == 0;\n",
        idx);
    if (c->type == DS_BOOL) {
        sb_appendf(out,
            "      if (isNull) r.%s = null;\n"
            "      else { byte b = ((byte*)c->Buffers[1])[rowOff/8];\n"
            "             r.%s = ((b >> (int)(rowOff&7)) & 1) != 0; }\n",
            c->name, c->name);
    } else if (c->type == DS_UTF8) {
        sb_appendf(out,
            "      if (isNull) r.%s = null;\n"
            "      else { int *offs = (int*)c->Buffers[1];\n"
            "             byte *data = (byte*)c->Buffers[2];\n"
            "             int s = offs[rowOff], e = offs[rowOff+1];\n"
            "             r.%s = System.Text.Encoding.UTF8.GetString(data + s, e - s); }\n",
            c->name, c->name);
    } else {
        /* All fixed-width numerics: read via the typed pointer at
         * rowOff. Narrow types auto-widen on assignment into r.X
         * because the field is nullable<narrow>; the C# compiler
         * inserts the implicit conversion. */
        const char *cs    = ds_cs_type(c->type);   /* e.g. "int?" */
        const char *ptr_t = ds_cs_buffer_ptr_type(c->type);
        sb_appendf(out,
            "      r.%s = isNull ? (%s)null : ((%s*)c->Buffers[1])[rowOff];\n",
            c->name, cs, ptr_t);
    }
    sb_append(out, "    }\n");
    return 0;
}

/* Emit the per-column output write for one cell. */
static int gen_output_write(Sbuf *out, const DsCol *c, size_t idx) {
    if (c->type == DS_BOOL) {
        sb_appendf(out,
            "    if (row.%s.HasValue) SetBoolFn(emitCtx, %zu, (byte)(row.%s.Value ? 1 : 0));\n"
            "    else SetNullFn(emitCtx, %zu);\n",
            c->name, idx, c->name, idx);
    } else if (c->type == DS_UTF8) {
        sb_appendf(out,
            "    if (row.%s != null) { var b = System.Text.Encoding.UTF8.GetBytes(row.%s);\n"
            "                          fixed (byte *p = b) SetUtf8Fn(emitCtx, %zu, p, b.Length); }\n"
            "    else SetNullFn(emitCtx, %zu);\n",
            c->name, c->name, idx, idx);
    } else if (ds_type_is_f64_stored(c->type)) {
        /* DT_R8 direct; DT_R4 widens (double)(float) is implicit but
         * we cast explicitly to avoid an analyzer warning. */
        sb_appendf(out,
            "    if (row.%s.HasValue) SetFloat64Fn(emitCtx, %zu, (double)row.%s.Value);\n"
            "    else SetNullFn(emitCtx, %zu);\n",
            c->name, idx, c->name, idx);
    } else {
        /* All integer widths route through SetInt64Fn — staging is
         * widened to long, narrowed at finalize. */
        sb_appendf(out,
            "    if (row.%s.HasValue) SetInt64Fn(emitCtx, %zu, (long)row.%s.Value);\n"
            "    else SetNullFn(emitCtx, %zu);\n",
            c->name, idx, c->name, idx);
    }
    return 0;
}

static int generate_cs(DotnetScript *s, Sbuf *out) {
    sb_append(out,
        "// AUTO-GENERATED by betl-dotnet. Do not edit.\n"
        "using System;\n"
        "namespace Betl;\n\n");
    /* InputRow */
    sb_append(out, "public partial class InputRow {\n");
    for (size_t i = 0; i < s->n_in; ++i) {
        sb_appendf(out, "  public %s %s { get; set; }\n",
                   ds_cs_type(s->in_cols[i].type), s->in_cols[i].name);
    }
    sb_append(out, "}\n\n");
    /* OutputRow */
    sb_append(out, "public partial class OutputRow {\n");
    for (size_t i = 0; i < s->n_out; ++i) {
        sb_appendf(out, "  public %s %s { get; set; }\n",
                   ds_cs_type(s->out_cols[i].type), s->out_cols[i].name);
    }
    sb_append(out, "}\n\n");
    /* Dispatch partial methods */
    sb_append(out, "internal static unsafe partial class Dispatch {\n");
    sb_append(out,
        "  internal static partial InputRow ExtractInputRow(ArrowArray* batch, long row) {\n"
        "    var r = new InputRow();\n");
    for (size_t i = 0; i < s->n_in; ++i) gen_input_extract(out, &s->in_cols[i], i);
    sb_append(out, "    return r;\n  }\n\n");

    sb_append(out,
        "  internal static partial void WriteOutputRow(IntPtr emitCtx, OutputRow row) {\n");
    for (size_t i = 0; i < s->n_out; ++i) gen_output_write(out, &s->out_cols[i], i);
    sb_append(out, "    CommitRowFn(emitCtx);\n  }\n");
    sb_append(out, "}\n");
    return 0;
}

/* --- output staging ------------------------------------------- */

static int ds_out_reserve(DsOutCol *c, size_t want) {
    if (want <= c->cap) return 0;
    size_t nc = c->cap ? c->cap : 64;
    while (nc < want) nc *= 2;
    uint8_t *nn = realloc(c->nulls, nc);
    if (!nn) return -1;
    c->nulls = nn;
    memset(c->nulls + c->cap, 0, nc - c->cap);
    if (ds_type_is_int64_stored(c->type)) {
        int64_t *p = realloc(c->i64_vals, nc * sizeof *p);
        if (!p) return -1;
        c->i64_vals = p;
    } else if (ds_type_is_f64_stored(c->type)) {
        double *p = realloc(c->f64_vals, nc * sizeof *p);
        if (!p) return -1;
        c->f64_vals = p;
    } else if (c->type == DS_BOOL) {
        uint8_t *p = realloc(c->b_vals, nc);
        if (!p) return -1;
        c->b_vals = p;
    } else if (c->type == DS_UTF8) {
        int32_t *po = realloc(c->u8_offsets, (nc + 1) * sizeof *po);
        if (!po) return -1;
        c->u8_offsets = po;
        if (c->cap == 0) c->u8_offsets[0] = 0;
        if (c->u8_cap == 0) {
            c->u8_cap = 128;
            c->u8_data = malloc(c->u8_cap);
            if (!c->u8_data) return -1;
        }
    }
    c->cap = nc;
    return 0;
}

static void ds_out_free(DsOutCol *c) {
    free(c->nulls); free(c->i64_vals); free(c->f64_vals);
    free(c->b_vals); free(c->u8_offsets); free(c->u8_data);
    memset(c, 0, sizeof *c);
}

/* --- Per-cell setter callbacks (called from C# via emit_ctx) ----
 *
 * The opaque ctx is a DsEmitter* — stack-built by the C caller at
 * each process_batch / on_eof call so the same setters can serve
 * any kind (script, pipelinecomponent, …) without per-kind casts. */

static void emit_set_int64(void *ctx, int idx, int64_t v) {
    DsEmitter *e = ctx;
    DsOutCol *c = &e->out_staging[idx];
    if (ds_out_reserve(c, c->n + 1) != 0) return;
    c->i64_vals[c->n] = v;
    e->pending_set[idx] = 1;
}
static void emit_set_float64(void *ctx, int idx, double v) {
    DsEmitter *e = ctx;
    DsOutCol *c = &e->out_staging[idx];
    if (ds_out_reserve(c, c->n + 1) != 0) return;
    c->f64_vals[c->n] = v;
    e->pending_set[idx] = 1;
}
static void emit_set_bool(void *ctx, int idx, uint8_t v) {
    DsEmitter *e = ctx;
    DsOutCol *c = &e->out_staging[idx];
    if (ds_out_reserve(c, c->n + 1) != 0) return;
    c->b_vals[c->n] = v ? 1 : 0;
    e->pending_set[idx] = 1;
}
static void emit_set_utf8(void *ctx, int idx, const uint8_t *str, int len) {
    DsEmitter *e = ctx;
    DsOutCol *c = &e->out_staging[idx];
    if (ds_out_reserve(c, c->n + 1) != 0) return;
    size_t need = c->u8_len + (size_t)len;
    if (need > c->u8_cap) {
        size_t nc = c->u8_cap;
        while (nc < need) nc *= 2;
        char *nd = realloc(c->u8_data, nc);
        if (!nd) return;
        c->u8_data = nd; c->u8_cap = nc;
    }
    if (len > 0) memcpy(c->u8_data + c->u8_len, str, (size_t)len);
    c->u8_len += (size_t)len;
    c->u8_offsets[c->n + 1] = (int32_t)c->u8_len;
    e->pending_set[idx] = 1;
}
static void emit_set_null(void *ctx, int idx) {
    DsEmitter *e = ctx;
    DsOutCol *c = &e->out_staging[idx];
    if (ds_out_reserve(c, c->n + 1) != 0) return;
    c->nulls[c->n] = 1;
    /* For utf8 we still need to advance the offset entry across NULL
     * rows so finalize doesn't end up with a discontinuity. */
    if (c->type == DS_UTF8) c->u8_offsets[c->n + 1] = (int32_t)c->u8_len;
    e->pending_set[idx] = 1;
}
static void emit_commit_row(void *ctx) {
    DsEmitter *e = ctx;
    /* Any column the script didn't set falls back to NULL. */
    for (size_t i = 0; i < e->n_out; ++i) {
        if (!e->pending_set[i]) {
            DsOutCol *c = &e->out_staging[i];
            if (ds_out_reserve(c, c->n + 1) != 0) continue;
            c->nulls[c->n] = 1;
            if (c->type == DS_UTF8) c->u8_offsets[c->n + 1] = (int32_t)c->u8_len;
        }
        e->pending_set[i] = 0;
        ++e->out_staging[i].n;
    }
}

/* Helper to build a transient DsEmitter from a DotnetScript or
 * DotnetPipelineComponent. Caller stack-allocates, fills in via
 * the macro, passes its address as emit_ctx. */
#define DS_EMITTER_OF(self) \
    (DsEmitter){ .out_staging = (self)->out_staging, \
                 .pending_set = (self)->pending_set, \
                 .n_out       = (self)->n_out }

/* --- finalize output batch ------------------------------------ */

static void ds_release_leaf2(struct ArrowArray *a) {
    if (a->n_buffers >= 2 && a->buffers) {
        free((void *)a->buffers[0]); free((void *)a->buffers[1]);
    }
    free(a->buffers); a->release = NULL;
}
static void ds_release_leaf3(struct ArrowArray *a) {
    if (a->n_buffers >= 3 && a->buffers) {
        free((void *)a->buffers[0]); free((void *)a->buffers[1]); free((void *)a->buffers[2]);
    }
    free(a->buffers); a->release = NULL;
}
static void ds_release_struct(struct ArrowArray *a) {
    for (int64_t i = 0; i < a->n_children; ++i) {
        if (a->children[i]) {
            if (a->children[i]->release) a->children[i]->release(a->children[i]);
            free(a->children[i]);
        }
    }
    free(a->children); free(a->buffers); a->release = NULL;
}

/* Narrow the widened i64/f64 storage into a typed Arrow buffer of
 * `elem_size` bytes per row. Caller owns the returned buffer. The
 * widened source is freed unconditionally on success. */
static void *ds_narrow_int(int64_t *src, size_t n, DsType t) {
    if (!src && n > 0) return NULL;
    switch (t) {
        case DS_INT8: case DS_UINT8: {
            uint8_t *p = malloc(n ? n : 1);
            if (!p) return NULL;
            for (size_t i = 0; i < n; ++i) p[i] = (uint8_t)src[i];
            return p;
        }
        case DS_INT16: case DS_UINT16: {
            uint16_t *p = malloc((n ? n : 1) * sizeof *p);
            if (!p) return NULL;
            for (size_t i = 0; i < n; ++i) p[i] = (uint16_t)src[i];
            return p;
        }
        case DS_INT32: case DS_UINT32: {
            uint32_t *p = malloc((n ? n : 1) * sizeof *p);
            if (!p) return NULL;
            for (size_t i = 0; i < n; ++i) p[i] = (uint32_t)src[i];
            return p;
        }
        default:
            return NULL;
    }
}
static float *ds_narrow_float32(double *src, size_t n) {
    float *p = malloc((n ? n : 1) * sizeof *p);
    if (!p) return NULL;
    for (size_t i = 0; i < n; ++i) p[i] = (float)src[i];
    return p;
}

static int ds_finalize_col(DsOutCol *c, struct ArrowArray *out) {
    size_t n = c->n;
    int64_t null_count = 0;
    uint8_t *vmap = NULL;
    for (size_t i = 0; i < n; ++i) if (c->nulls[i]) ++null_count;
    if (null_count > 0) {
        size_t bytes = (n + 7) / 8;
        vmap = malloc(bytes ? bytes : 1);
        if (!vmap) return -1;
        memset(vmap, 0xFF, bytes ? bytes : 1);
        for (size_t i = 0; i < n; ++i) {
            if (c->nulls[i]) vmap[i / 8] &= (uint8_t)~(1u << (i % 8));
        }
    }
    free(c->nulls); c->nulls = NULL;
    /* Wide passthrough: int64 / uint64 / float64 own the wide buffer
     * and hand it straight to Arrow (DS_UINT64 is bit-identical to
     * DS_INT64 here — i64_vals already holds the right 64 bits). */
    if (c->type == DS_INT64 || c->type == DS_UINT64 || c->type == DS_FLOAT64) {
        const void **bufs = malloc(2 * sizeof *bufs);
        if (!bufs) { free(vmap); return -1; }
        bufs[0] = vmap;
        bufs[1] = (c->type == DS_FLOAT64) ? (void *)c->f64_vals : (void *)c->i64_vals;
        c->i64_vals = NULL; c->f64_vals = NULL;
        out->length = (int64_t)n; out->null_count = null_count;
        out->n_buffers = 2; out->buffers = bufs; out->release = ds_release_leaf2;
        return 0;
    }
    /* Narrow numerics: allocate a typed buffer, narrow into it, free
     * the wide source. Same Arrow leaf shape (validity + values). */
    if (ds_type_is_int64_stored(c->type)) {
        void *narrow = ds_narrow_int(c->i64_vals, n, c->type);
        if (!narrow) { free(vmap); return -1; }
        free(c->i64_vals); c->i64_vals = NULL;
        const void **bufs = malloc(2 * sizeof *bufs);
        if (!bufs) { free(vmap); free(narrow); return -1; }
        bufs[0] = vmap; bufs[1] = narrow;
        out->length = (int64_t)n; out->null_count = null_count;
        out->n_buffers = 2; out->buffers = bufs; out->release = ds_release_leaf2;
        return 0;
    }
    if (c->type == DS_FLOAT32) {
        float *narrow = ds_narrow_float32(c->f64_vals, n);
        if (!narrow) { free(vmap); return -1; }
        free(c->f64_vals); c->f64_vals = NULL;
        const void **bufs = malloc(2 * sizeof *bufs);
        if (!bufs) { free(vmap); free(narrow); return -1; }
        bufs[0] = vmap; bufs[1] = narrow;
        out->length = (int64_t)n; out->null_count = null_count;
        out->n_buffers = 2; out->buffers = bufs; out->release = ds_release_leaf2;
        return 0;
    }
    if (c->type == DS_BOOL) {
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
        out->n_buffers = 2; out->buffers = bufs; out->release = ds_release_leaf2;
        return 0;
    }
    /* UTF8: smooth offsets across nulls (the setter sets offset even
     * for nulls, but if a row was forgotten by commit_row's fallback
     * we may have gaps). */
    int32_t last = 0;
    for (size_t i = 1; i <= n; ++i) {
        if (c->u8_offsets[i] < last) c->u8_offsets[i] = last;
        else last = c->u8_offsets[i];
    }
    int32_t *offs = c->u8_offsets; c->u8_offsets = NULL;
    char *data = c->u8_data; c->u8_data = NULL;
    if (!data) { data = malloc(1); if (!data) { free(vmap); free(offs); return -1; } }
    const void **bufs = malloc(3 * sizeof *bufs);
    if (!bufs) { free(vmap); free(offs); free(data); return -1; }
    bufs[0] = vmap; bufs[1] = offs; bufs[2] = data;
    out->length = (int64_t)n; out->null_count = null_count;
    out->n_buffers = 3; out->buffers = bufs; out->release = ds_release_leaf3;
    return 0;
}

/* --- compile + load ------------------------------------------ */

/* Build the cache-key hash input from the (input + output) schema —
 * NULs separating fields keep collisions unlikely. */
static char *build_schema_hash_input(DotnetScript *s, size_t *out_len) {
    Sbuf sb = {0};
    for (size_t i = 0; i < s->n_in; ++i)
        sb_appendf(&sb, "in:%s:%c\n", s->in_cols[i].name, s->in_cols[i].arrow_fmt);
    for (size_t i = 0; i < s->n_out; ++i)
        sb_appendf(&sb, "out:%s:%c\n", s->out_cols[i].name, s->out_cols[i].arrow_fmt);
    *out_len = sb.len;
    return sb.p;
}

static int compile_and_load(DotnetScript *s) {
    if (s->compiled) return 0;
    Sbuf gen = {0};
    if (generate_cs(s, &gen) != 0) {
        ds_set_err(s, "dotnet.script: codegen failed");
        free(gen.p); return -1;
    }
    size_t hash_len = 0;
    char *hash_in = build_schema_hash_input(s, &hash_len);

    extra_file_t ef = { "Generated.cs", gen.p, gen.len };
    compile_request_t creq = {
        .user_source    = s->source,
        .lang           = s->lang,
        .kind           = "script",
        .extra_files    = &ef,
        .n_extra        = 1,
        .shim_subdir    = "script",
        .extra_hash_in  = hash_in,
        .extra_hash_len = hash_len,
    };
    char err[8192] = {0};
    if (compile_to_cache(s->ctx, &creq, &s->so_path, err, sizeof err) != 0) {
        ds_set_err(s, "dotnet.script: compile failed: %s", err);
        free(gen.p); free(hash_in);
        return -1;
    }
    free(gen.p); free(hash_in);

    s->handle = dlopen(s->so_path, RTLD_NOW | RTLD_LOCAL);
    if (!s->handle) {
        ds_set_err(s, "dotnet.script: dlopen(%s) failed: %s",
                   s->so_path, dlerror());
        return -1;
    }
    *(void **)&s->init_fn           = dlsym(s->handle, "betl_dotnet_init");
    *(void **)&s->script_init_fn    = dlsym(s->handle, "betl_dotnet_script_init");
    *(void **)&s->register_emit_fn  = dlsym(s->handle, "betl_dotnet_script_register_emit");
    *(void **)&s->process_batch_fn  = dlsym(s->handle, "betl_dotnet_script_process_batch");
    *(void **)&s->on_eof_fn         = dlsym(s->handle, "betl_dotnet_script_on_eof");
    if (!s->init_fn || !s->script_init_fn || !s->register_emit_fn
        || !s->process_batch_fn || !s->on_eof_fn) {
        ds_set_err(s, "dotnet.script: AOT'd .so is missing required entry points");
        return -1;
    }
    if (s->init_fn(s->ctx, host_log_wrapper, host_get_param_wrapper,
                   host_get_connection_wrapper) != 0) {
        ds_set_err(s, "dotnet.script: managed init returned non-zero");
        return -1;
    }
    if (s->register_emit_fn(emit_set_int64, emit_set_float64, emit_set_bool,
                            emit_set_utf8, emit_set_null, emit_commit_row) != 0) {
        ds_set_err(s, "dotnet.script: register_emit returned non-zero");
        return -1;
    }
    if (s->script_init_fn() != 0) {
        ds_set_err(s, "dotnet.script: UserScript constructor threw");
        return -1;
    }

    /* Allocate output staging now we know n_out is final. */
    s->out_staging = calloc(s->n_out, sizeof *s->out_staging);
    s->pending_set = calloc(s->n_out, sizeof *s->pending_set);
    if (!s->out_staging || !s->pending_set) {
        ds_set_err(s, "dotnet.script: OOM");
        return -1;
    }
    for (size_t i = 0; i < s->n_out; ++i)
        s->out_staging[i].type = s->out_cols[i].type;

    s->compiled = 1;
    return 0;
}

/* --- Arrow stream callbacks ----------------------------------- */

static void ds_release_schema_named(struct ArrowSchema *sch) {
    free((void *)sch->name); sch->name = NULL; sch->release = NULL;
}
static void ds_release_schema_struct(struct ArrowSchema *sch) {
    for (int64_t i = 0; i < sch->n_children; ++i) {
        if (sch->children[i]) {
            if (sch->children[i]->release) sch->children[i]->release(sch->children[i]);
            free(sch->children[i]);
        }
    }
    free(sch->children); sch->release = NULL;
}

static int ds_get_schema(struct ArrowArrayStream *st, struct ArrowSchema *out) {
    DotnetScript *s = st->private_data;
    memset(out, 0, sizeof *out);
    struct ArrowSchema **kids = calloc(s->n_out, sizeof *kids);
    if (!kids) return ENOMEM;
    for (size_t i = 0; i < s->n_out; ++i) {
        struct ArrowSchema *c = calloc(1, sizeof *c);
        char *nm = strdup(s->out_cols[i].name);
        if (!c || !nm) {
            free(c); free(nm);
            for (size_t k = 0; k < i; ++k) {
                if (kids[k]->release) kids[k]->release(kids[k]);
                free(kids[k]);
            }
            free(kids); return ENOMEM;
        }
        c->name = nm; c->flags = ARROW_FLAG_NULLABLE;
        switch (s->out_cols[i].type) {
            case DS_INT64:   c->format = "l"; break;
            case DS_FLOAT64: c->format = "g"; break;
            case DS_BOOL:    c->format = "b"; break;
            case DS_UTF8:    c->format = "u"; break;
            case DS_INT8:    c->format = "c"; break;
            case DS_INT16:   c->format = "s"; break;
            case DS_INT32:   c->format = "i"; break;
            case DS_UINT8:   c->format = "C"; break;
            case DS_UINT16:  c->format = "S"; break;
            case DS_UINT32:  c->format = "I"; break;
            case DS_UINT64:  c->format = "L"; break;
            case DS_FLOAT32: c->format = "f"; break;
        }
        c->release = ds_release_schema_named;
        kids[i] = c;
    }
    out->format = "+s"; out->n_children = (int64_t)s->n_out;
    out->children = kids; out->release = ds_release_schema_struct;
    return 0;
}

static int ds_flush_batch(DotnetScript *s, struct ArrowArray *out) {
    size_t n = s->out_staging[0].n;
    /* Sanity check: all output cols should have the same row count. */
    for (size_t i = 1; i < s->n_out; ++i) {
        if (s->out_staging[i].n != n) {
            ds_set_err(s, "dotnet.script: output column row count mismatch "
                          "(col 0 = %zu, col %zu = %zu)",
                       n, i, s->out_staging[i].n);
            return -1;
        }
    }
    struct ArrowArray **kids = calloc(s->n_out, sizeof *kids);
    if (!kids) return -1;
    for (size_t i = 0; i < s->n_out; ++i) {
        kids[i] = calloc(1, sizeof **kids);
        if (!kids[i] || ds_finalize_col(&s->out_staging[i], kids[i]) != 0) {
            for (size_t k = 0; k <= i; ++k) {
                if (kids[k]) {
                    if (kids[k]->release) kids[k]->release(kids[k]);
                    free(kids[k]);
                }
            }
            free(kids); return -1;
        }
    }
    const void **outer = malloc(sizeof *outer);
    if (!outer) {
        for (size_t i = 0; i < s->n_out; ++i) {
            if (kids[i]->release) kids[i]->release(kids[i]);
            free(kids[i]);
        }
        free(kids); return -1;
    }
    outer[0] = NULL;
    out->length = (int64_t)n; out->null_count = 0;
    out->n_buffers = 1; out->n_children = (int64_t)s->n_out;
    out->buffers = outer; out->children = kids;
    out->release = ds_release_struct;
    /* Reset for next batch. */
    for (size_t i = 0; i < s->n_out; ++i) {
        ds_out_free(&s->out_staging[i]);
        s->out_staging[i].type = s->out_cols[i].type;
    }
    return 0;
}

static int ds_get_next(struct ArrowArrayStream *st, struct ArrowArray *out) {
    DotnetScript *s = st->private_data;
    memset(out, 0, sizeof *out);
    if (cache_input_schema(s) != 0) return EIO;
    if (compile_and_load(s) != 0)   return EIO;
    DsEmitter em = DS_EMITTER_OF(s);

    /* Pull batches until at least one output row is buffered, or EOF
     * (then run on_eof and emit whatever it produces). */
    while (s->out_staging[0].n == 0) {
        if (s->eof_seen) return 0;
        struct ArrowArray in_arr = {0};
        if (s->input.get_next(&s->input, &in_arr) != 0) {
            ds_set_err(s, "dotnet.script: upstream get_next failed");
            return EIO;
        }
        if (!in_arr.release) {
            s->eof_seen = 1;
            if (s->on_eof_fn(&em) != 0) {
                ds_set_err(s, "dotnet.script: on_eof returned non-zero "
                              "(see [ERROR] log above)");
                return EIO;
            }
            if (s->out_staging[0].n == 0) return 0;
            break;
        }
        if (s->process_batch_fn(&in_arr, &em) != 0) {
            in_arr.release(&in_arr);
            ds_set_err(s, "dotnet.script: process_batch returned non-zero");
            return EIO;
        }
        in_arr.release(&in_arr);
    }
    if (ds_flush_batch(s, out) != 0) {
        ds_set_err(s, "dotnet.script: flush failed");
        return EIO;
    }
    return 0;
}

static const char *ds_get_last_error(struct ArrowArrayStream *st) {
    DotnetScript *s = st->private_data;
    return (s && s->last_err[0]) ? s->last_err : NULL;
}
static void ds_release(struct ArrowArrayStream *st) {
    st->private_data = NULL; st->release = NULL;
}

static int dotnet_script_init(BetlContext *ctx, const char *cfg, void **state) {
    DotnetScript *s = calloc(1, sizeof *s);
    if (!s) return BETL_ERR_INTERNAL;
    s->ctx = ctx;
    if (json_get_string(cfg, "source", &s->source) != 0 || !s->source) {
        betl_set_error(ctx, "dotnet.script: 'source' is required");
        free(s); return BETL_ERR_INVALID;
    }
    if (json_get_string(cfg, "lang", &s->lang) != 0 || !s->lang) {
        s->lang = strdup("csharp");
    }
    if (strcmp(s->lang, "csharp") != 0) {
        betl_set_error(ctx,
            "dotnet.script: lang '%s' is not supported; "
            "VB.NET is handled by the DTSX converter (VB → C#)", s->lang);
        free(s->source); free(s->lang); free(s);
        return BETL_ERR_UNSUPPORTED;
    }
    if (parse_output_schema(s, cfg) != 0) {
        for (size_t i = 0; i < s->n_out; ++i) free(s->out_cols[i].name);
        free(s->out_cols); free(s->source); free(s->lang); free(s);
        return BETL_ERR_INVALID;
    }
    *state = s;
    return BETL_OK;
}

static int dotnet_script_attach_input(void *state, int port,
                                      struct ArrowArrayStream *in) {
    (void)port;
    DotnetScript *s = state;
    s->input = *in;
    s->have_input = 1;
    memset(in, 0, sizeof *in);
    return BETL_OK;
}

static int dotnet_script_attach_output(void *state, int port,
                                       struct ArrowArrayStream *out) {
    (void)port;
    DotnetScript *s = state;
    out->get_schema     = ds_get_schema;
    out->get_next       = ds_get_next;
    out->get_last_error = ds_get_last_error;
    out->release        = ds_release;
    out->private_data   = s;
    return BETL_OK;
}

static void dotnet_script_destroy(void *state) {
    if (!state) return;
    DotnetScript *s = state;
    if (s->have_input && s->input.release) s->input.release(&s->input);
    /* See dotnet_task_destroy for the dlclose-leak rationale. */
    free(s->so_path);
    free(s->source); free(s->lang);
    for (size_t i = 0; i < s->n_out; ++i) free(s->out_cols[i].name);
    free(s->out_cols);
    for (size_t i = 0; i < s->n_in; ++i) free(s->in_cols[i].name);
    free(s->in_cols);
    if (s->out_staging) {
        for (size_t i = 0; i < s->n_out; ++i) ds_out_free(&s->out_staging[i]);
        free(s->out_staging);
    }
    free(s->pending_set);
    free(s);
}

static const BetlPortDef ds_inputs[]  = {
    { .name = "in",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "rows to process" },
};
static const BetlPortDef ds_outputs[] = {
    { .name = "out", .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "rows emitted by the script" },
};


/* ============================================================== *
 *  dotnet.pipelinecomponent — Phase 1a (sync transforms, types    *
 *  l/g/b/u, single column space = output_schema)                  *
 *                                                                  *
 *  Parallel to dotnet.script but with the SSIS PipelineComponent  *
 *  lifecycle (PreExecute / ProcessInput / PostExecute / Cleanup). *
 *  Reuses the script's DsCol / DsOutCol staging machinery via the *
 *  generic ds_* helpers; only the setters, the C# entry-point    *
 *  function pointers, and the per-step lifecycle loop differ.    *
 *  A separate struct keeps the two flows decoupled.               *
 * ============================================================== */

typedef int (*dpc_register_emit_fn)(
    void (*set_int64)  (void *, int, int64_t),
    void (*set_float64)(void *, int, double),
    void (*set_bool)   (void *, int, uint8_t),
    void (*set_utf8)   (void *, int, const uint8_t *, int),
    void (*set_null)   (void *, int),
    void (*commit_row) (void *));
typedef int (*dpc_register_schema_fn)(
    const char **input_names,  const char *input_fmts,  int n_input,
    const char **output_names, const char *output_fmts, int n_output);
typedef int (*dpc_lifecycle0_fn)(void);
typedef int (*dpc_process_batch_fn)(struct ArrowArray *batch, void *emit_ctx);

typedef struct {
    BetlContext             *ctx;
    char                    *source;
    char                    *lang;
    /* output_schema parsed from YAML */
    DsCol                   *out_cols;
    size_t                   n_out;
    /* input_schema cached from upstream on first get_next */
    DsCol                   *in_cols;
    size_t                   n_in;

    /* dlopen / function pointers */
    char                    *so_path;
    void                    *handle;
    ds_init_fn               init_fn;
    dpc_register_emit_fn     register_emit_fn;
    dpc_register_schema_fn   register_schema_fn;
    dpc_lifecycle0_fn        pc_init_fn;
    dpc_lifecycle0_fn        pre_execute_fn;
    dpc_process_batch_fn     process_batch_fn;
    dpc_lifecycle0_fn        post_execute_fn;
    dpc_lifecycle0_fn        cleanup_fn;

    struct ArrowArrayStream  input;
    int                      have_input;
    int                      compiled;
    int                      eof_seen;
    int                      post_executed;

    DsOutCol                *out_staging;
    int                     *pending_set;
    char                     last_err[1024];
} DotnetPipelineComponent;

static void dpc_set_err(DotnetPipelineComponent *p, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(p->last_err, sizeof p->last_err, fmt, ap); va_end(ap);
    betl_set_error(p->ctx, "%s", p->last_err);
}

/* ---- output_schema parsing (mirrors parse_output_schema for script) -- */

static int dpc_parse_output_schema(DotnetPipelineComponent *p, const char *cfg) {
    const char *q = json_value_after(cfg, "output_schema");
    if (!q || *q != '[') {
        dpc_set_err(p, "dotnet.pipelinecomponent: 'output_schema' is required (array)");
        return -1;
    }
    ++q;
    while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') ++q;
    if (*q == ']') {
        dpc_set_err(p, "dotnet.pipelinecomponent: 'output_schema' is empty");
        return -1;
    }
    for (;;) {
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') ++q;
        if (*q != '{') {
            dpc_set_err(p, "dotnet.pipelinecomponent: output_schema entry must be an object");
            return -1;
        }
        const char *start = q;
        int depth = 0;
        for (; *q; ++q) {
            if (*q == '"') {
                ++q;
                while (*q && *q != '"') { if (*q == '\\' && q[1]) ++q; ++q; }
                if (!*q) {
                    dpc_set_err(p, "dotnet.pipelinecomponent: malformed output_schema");
                    return -1;
                }
                continue;
            }
            if (*q == '{') ++depth;
            else if (*q == '}') { --depth; if (depth == 0) { ++q; break; } }
        }
        size_t len = (size_t)(q - start);
        char *buf = malloc(len + 1);
        if (!buf) return -1;
        memcpy(buf, start, len); buf[len] = '\0';

        char *name = NULL, *type = NULL;
        json_get_string(buf, "name", &name);
        json_get_string(buf, "type", &type);
        free(buf);
        if (!name || !type) {
            dpc_set_err(p, "dotnet.pipelinecomponent: output column needs name + type");
            free(name); free(type); return -1;
        }
        DsType t;
        if (strlen(type) != 1 || ds_fmt_to_type(type[0], &t) != 0) {
            dpc_set_err(p,
                "dotnet.pipelinecomponent: output column '%s' type '%s' "
                "not supported (Phase 1b: l/g/b/u + c/C/s/S/i/I/L/f)", name, type);
            free(name); free(type); return -1;
        }
        free(type);
        DsCol *grow = realloc(p->out_cols, (p->n_out + 1) * sizeof *grow);
        if (!grow) { free(name); return -1; }
        p->out_cols = grow;
        p->out_cols[p->n_out].name = name;
        p->out_cols[p->n_out].type = t;
        p->out_cols[p->n_out].arrow_fmt = ds_type_to_fmt(t);
        ++p->n_out;

        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') ++q;
        if (*q == ',') { ++q; continue; }
        if (*q == ']') return 0;
        dpc_set_err(p, "dotnet.pipelinecomponent: malformed output_schema array");
        return -1;
    }
}

/* ---- input schema caching (parallels cache_input_schema) ------------- */

static int dpc_cache_input_schema(DotnetPipelineComponent *p) {
    if (p->in_cols) return 0;
    struct ArrowSchema up = {0};
    if (p->input.get_schema(&p->input, &up) != 0) {
        dpc_set_err(p, "dotnet.pipelinecomponent: upstream get_schema failed");
        return -1;
    }
    int rc = -1;
    if (!up.format || strcmp(up.format, "+s") != 0 || up.n_children <= 0) {
        dpc_set_err(p, "dotnet.pipelinecomponent: input must be a struct");
        goto done;
    }
    size_t n = (size_t)up.n_children;
    DsCol *cols = calloc(n, sizeof *cols);
    if (!cols) { dpc_set_err(p, "dotnet.pipelinecomponent: OOM"); goto done; }
    for (size_t i = 0; i < n; ++i) {
        struct ArrowSchema *c = up.children[i];
        const char *fmt = c && c->format ? c->format : "";
        DsType t;
        if (strlen(fmt) != 1 || ds_fmt_to_type(fmt[0], &t) != 0) {
            dpc_set_err(p,
                "dotnet.pipelinecomponent: input column '%s' has unsupported "
                "Arrow format '%s' (Phase 1b: l/g/b/u + c/C/s/S/i/I/L/f)",
                c && c->name ? c->name : "?", fmt);
            for (size_t k = 0; k < i; ++k) free(cols[k].name);
            free(cols); goto done;
        }
        cols[i].name = strdup(c && c->name ? c->name : "");
        cols[i].type = t;
        cols[i].arrow_fmt = fmt[0];
        if (!cols[i].name) {
            for (size_t k = 0; k < i; ++k) free(cols[k].name);
            free(cols); goto done;
        }
    }
    p->in_cols = cols;
    p->n_in = n;
    rc = 0;
done:
    if (up.release) up.release(&up);
    return rc;
}

/* Emit setters: shared with dotnet.script via DsEmitter. */

/* ---- compile + load ------------------------------------------------- */

static char *dpc_build_schema_hash_input(DotnetPipelineComponent *p, size_t *out_len) {
    Sbuf sb = {0};
    for (size_t i = 0; i < p->n_in; ++i)
        sb_appendf(&sb, "in:%s:%c\n", p->in_cols[i].name, p->in_cols[i].arrow_fmt);
    for (size_t i = 0; i < p->n_out; ++i)
        sb_appendf(&sb, "out:%s:%c\n", p->out_cols[i].name, p->out_cols[i].arrow_fmt);
    *out_len = sb.len;
    return sb.p;
}

static int dpc_compile_and_load(DotnetPipelineComponent *p) {
    if (p->compiled) return 0;

    size_t hash_len = 0;
    char *hash_in = dpc_build_schema_hash_input(p, &hash_len);

    compile_request_t creq = {
        .user_source    = p->source,
        .lang           = p->lang,
        .kind           = "pc",
        .extra_files    = NULL,
        .n_extra        = 0,
        .shim_subdir    = "pipelinecomponent",
        .extra_hash_in  = hash_in,
        .extra_hash_len = hash_len,
    };
    char err[8192] = {0};
    if (compile_to_cache(p->ctx, &creq, &p->so_path, err, sizeof err) != 0) {
        dpc_set_err(p, "dotnet.pipelinecomponent: compile failed: %s", err);
        free(hash_in);
        return -1;
    }
    free(hash_in);

    p->handle = dlopen(p->so_path, RTLD_NOW | RTLD_LOCAL);
    if (!p->handle) {
        dpc_set_err(p, "dotnet.pipelinecomponent: dlopen(%s) failed: %s",
                    p->so_path, dlerror());
        return -1;
    }
    *(void **)&p->init_fn            = dlsym(p->handle, "betl_dotnet_init");
    *(void **)&p->register_emit_fn   = dlsym(p->handle, "betl_dotnet_pc_register_emit");
    *(void **)&p->register_schema_fn = dlsym(p->handle, "betl_dotnet_pc_register_schema");
    *(void **)&p->pc_init_fn         = dlsym(p->handle, "betl_dotnet_pc_init");
    *(void **)&p->pre_execute_fn     = dlsym(p->handle, "betl_dotnet_pc_pre_execute");
    *(void **)&p->process_batch_fn   = dlsym(p->handle, "betl_dotnet_pc_process_batch");
    *(void **)&p->post_execute_fn    = dlsym(p->handle, "betl_dotnet_pc_post_execute");
    *(void **)&p->cleanup_fn         = dlsym(p->handle, "betl_dotnet_pc_cleanup");
    if (!p->init_fn || !p->register_emit_fn || !p->register_schema_fn
        || !p->pc_init_fn || !p->pre_execute_fn || !p->process_batch_fn
        || !p->post_execute_fn || !p->cleanup_fn) {
        dpc_set_err(p, "dotnet.pipelinecomponent: AOT'd .so is missing required entry points");
        return -1;
    }
    if (p->init_fn(p->ctx, host_log_wrapper, host_get_param_wrapper,
                   host_get_connection_wrapper) != 0) {
        dpc_set_err(p, "dotnet.pipelinecomponent: managed init returned non-zero");
        return -1;
    }
    if (p->register_emit_fn(emit_set_int64, emit_set_float64, emit_set_bool,
                            emit_set_utf8, emit_set_null, emit_commit_row) != 0) {
        dpc_set_err(p, "dotnet.pipelinecomponent: register_emit returned non-zero");
        return -1;
    }
    /* Hand input + output schema descriptors to the shim. */
    const char **in_names  = calloc(p->n_in,  sizeof *in_names);
    const char **out_names = calloc(p->n_out, sizeof *out_names);
    char *in_fmts  = calloc(p->n_in  + 1, 1);
    char *out_fmts = calloc(p->n_out + 1, 1);
    if (!in_names || !out_names || !in_fmts || !out_fmts) {
        free(in_names); free(out_names); free(in_fmts); free(out_fmts);
        dpc_set_err(p, "dotnet.pipelinecomponent: OOM");
        return -1;
    }
    for (size_t i = 0; i < p->n_in;  ++i) {
        in_names[i]  = p->in_cols[i].name;
        in_fmts[i]   = p->in_cols[i].arrow_fmt;
    }
    for (size_t i = 0; i < p->n_out; ++i) {
        out_names[i] = p->out_cols[i].name;
        out_fmts[i]  = p->out_cols[i].arrow_fmt;
    }
    int rs = p->register_schema_fn(in_names, in_fmts, (int)p->n_in,
                                   out_names, out_fmts, (int)p->n_out);
    free(in_names); free(out_names); free(in_fmts); free(out_fmts);
    if (rs != 0) {
        dpc_set_err(p, "dotnet.pipelinecomponent: register_schema returned non-zero "
                       "(see [ERROR] log above)");
        return -1;
    }
    if (p->pc_init_fn() != 0) {
        dpc_set_err(p, "dotnet.pipelinecomponent: UserComponent constructor threw");
        return -1;
    }
    if (p->pre_execute_fn() != 0) {
        dpc_set_err(p, "dotnet.pipelinecomponent: PreExecute threw "
                       "(see [ERROR] log above)");
        return -1;
    }

    /* Allocate output staging. */
    p->out_staging = calloc(p->n_out, sizeof *p->out_staging);
    p->pending_set = calloc(p->n_out, sizeof *p->pending_set);
    if (!p->out_staging || !p->pending_set) {
        dpc_set_err(p, "dotnet.pipelinecomponent: OOM");
        return -1;
    }
    for (size_t i = 0; i < p->n_out; ++i)
        p->out_staging[i].type = p->out_cols[i].type;

    p->compiled = 1;
    return 0;
}

/* ---- Arrow stream callbacks ----------------------------------------- */

static int dpc_get_schema(struct ArrowArrayStream *st, struct ArrowSchema *out) {
    DotnetPipelineComponent *p = st->private_data;
    memset(out, 0, sizeof *out);
    struct ArrowSchema **kids = calloc(p->n_out, sizeof *kids);
    if (!kids) return ENOMEM;
    for (size_t i = 0; i < p->n_out; ++i) {
        struct ArrowSchema *c = calloc(1, sizeof *c);
        char *nm = strdup(p->out_cols[i].name);
        if (!c || !nm) {
            free(c); free(nm);
            for (size_t k = 0; k < i; ++k) {
                if (kids[k]->release) kids[k]->release(kids[k]);
                free(kids[k]);
            }
            free(kids); return ENOMEM;
        }
        c->name = nm; c->flags = ARROW_FLAG_NULLABLE;
        switch (p->out_cols[i].type) {
            case DS_INT64:   c->format = "l"; break;
            case DS_FLOAT64: c->format = "g"; break;
            case DS_BOOL:    c->format = "b"; break;
            case DS_UTF8:    c->format = "u"; break;
            case DS_INT8:    c->format = "c"; break;
            case DS_INT16:   c->format = "s"; break;
            case DS_INT32:   c->format = "i"; break;
            case DS_UINT8:   c->format = "C"; break;
            case DS_UINT16:  c->format = "S"; break;
            case DS_UINT32:  c->format = "I"; break;
            case DS_UINT64:  c->format = "L"; break;
            case DS_FLOAT32: c->format = "f"; break;
        }
        c->release = ds_release_schema_named;
        kids[i] = c;
    }
    out->format = "+s"; out->n_children = (int64_t)p->n_out;
    out->children = kids; out->release = ds_release_schema_struct;
    return 0;
}

static int dpc_flush_batch(DotnetPipelineComponent *p, struct ArrowArray *out) {
    size_t n = p->out_staging[0].n;
    for (size_t i = 1; i < p->n_out; ++i) {
        if (p->out_staging[i].n != n) {
            dpc_set_err(p, "dotnet.pipelinecomponent: output column row count mismatch "
                           "(col 0 = %zu, col %zu = %zu)",
                        n, i, p->out_staging[i].n);
            return -1;
        }
    }
    struct ArrowArray **kids = calloc(p->n_out, sizeof *kids);
    if (!kids) return -1;
    for (size_t i = 0; i < p->n_out; ++i) {
        kids[i] = calloc(1, sizeof **kids);
        if (!kids[i] || ds_finalize_col(&p->out_staging[i], kids[i]) != 0) {
            for (size_t k = 0; k <= i; ++k) {
                if (kids[k]) {
                    if (kids[k]->release) kids[k]->release(kids[k]);
                    free(kids[k]);
                }
            }
            free(kids); return -1;
        }
    }
    const void **outer = malloc(sizeof *outer);
    if (!outer) {
        for (size_t i = 0; i < p->n_out; ++i) {
            if (kids[i]->release) kids[i]->release(kids[i]);
            free(kids[i]);
        }
        free(kids); return -1;
    }
    outer[0] = NULL;
    out->length = (int64_t)n; out->null_count = 0;
    out->n_buffers = 1; out->n_children = (int64_t)p->n_out;
    out->buffers = outer; out->children = kids;
    out->release = ds_release_struct;
    for (size_t i = 0; i < p->n_out; ++i) {
        ds_out_free(&p->out_staging[i]);
        p->out_staging[i].type = p->out_cols[i].type;
    }
    return 0;
}

static int dpc_get_next(struct ArrowArrayStream *st, struct ArrowArray *out) {
    DotnetPipelineComponent *p = st->private_data;
    memset(out, 0, sizeof *out);
    if (dpc_cache_input_schema(p) != 0) return EIO;
    if (dpc_compile_and_load(p) != 0)   return EIO;
    DsEmitter em = DS_EMITTER_OF(p);

    while (p->out_staging[0].n == 0) {
        if (p->eof_seen) {
            if (!p->post_executed) {
                p->post_executed = 1;
                if (p->post_execute_fn() != 0) {
                    dpc_set_err(p, "dotnet.pipelinecomponent: PostExecute threw "
                                   "(see [ERROR] log above)");
                    return EIO;
                }
                if (p->cleanup_fn() != 0) {
                    dpc_set_err(p, "dotnet.pipelinecomponent: Cleanup threw "
                                   "(see [ERROR] log above)");
                    return EIO;
                }
            }
            return 0;
        }
        struct ArrowArray in_arr = {0};
        if (p->input.get_next(&p->input, &in_arr) != 0) {
            dpc_set_err(p, "dotnet.pipelinecomponent: upstream get_next failed");
            return EIO;
        }
        if (!in_arr.release) {
            p->eof_seen = 1;
            continue; /* fall through to PostExecute on next loop turn */
        }
        if (p->process_batch_fn(&in_arr, &em) != 0) {
            in_arr.release(&in_arr);
            dpc_set_err(p, "dotnet.pipelinecomponent: ProcessInput returned non-zero "
                           "(see [ERROR] log above)");
            return EIO;
        }
        in_arr.release(&in_arr);
    }
    if (dpc_flush_batch(p, out) != 0) {
        dpc_set_err(p, "dotnet.pipelinecomponent: flush failed");
        return EIO;
    }
    return 0;
}

static const char *dpc_get_last_error(struct ArrowArrayStream *st) {
    DotnetPipelineComponent *p = st->private_data;
    return (p && p->last_err[0]) ? p->last_err : NULL;
}
static void dpc_release(struct ArrowArrayStream *st) {
    st->private_data = NULL; st->release = NULL;
}

static int dotnet_pc_init(BetlContext *ctx, const char *cfg, void **state) {
    DotnetPipelineComponent *p = calloc(1, sizeof *p);
    if (!p) return BETL_ERR_INTERNAL;
    p->ctx = ctx;
    if (json_get_string(cfg, "source", &p->source) != 0 || !p->source) {
        betl_set_error(ctx, "dotnet.pipelinecomponent: 'source' is required");
        free(p); return BETL_ERR_INVALID;
    }
    if (json_get_string(cfg, "lang", &p->lang) != 0 || !p->lang) {
        p->lang = strdup("csharp");
    }
    if (strcmp(p->lang, "csharp") != 0) {
        betl_set_error(ctx,
            "dotnet.pipelinecomponent: lang '%s' is not supported; "
            "Phase 1a is C# only", p->lang);
        free(p->source); free(p->lang); free(p);
        return BETL_ERR_UNSUPPORTED;
    }
    if (dpc_parse_output_schema(p, cfg) != 0) {
        for (size_t i = 0; i < p->n_out; ++i) free(p->out_cols[i].name);
        free(p->out_cols); free(p->source); free(p->lang); free(p);
        return BETL_ERR_INVALID;
    }
    *state = p;
    return BETL_OK;
}

static int dotnet_pc_attach_input(void *state, int port,
                                  struct ArrowArrayStream *in) {
    (void)port;
    DotnetPipelineComponent *p = state;
    p->input = *in;
    p->have_input = 1;
    memset(in, 0, sizeof *in);
    return BETL_OK;
}

static int dotnet_pc_attach_output(void *state, int port,
                                   struct ArrowArrayStream *out) {
    (void)port;
    DotnetPipelineComponent *p = state;
    out->get_schema     = dpc_get_schema;
    out->get_next       = dpc_get_next;
    out->get_last_error = dpc_get_last_error;
    out->release        = dpc_release;
    out->private_data   = p;
    return BETL_OK;
}

static void dotnet_pc_destroy(void *state) {
    if (!state) return;
    DotnetPipelineComponent *p = state;
    if (p->have_input && p->input.release) p->input.release(&p->input);
    free(p->so_path);
    free(p->source); free(p->lang);
    for (size_t i = 0; i < p->n_out; ++i) free(p->out_cols[i].name);
    free(p->out_cols);
    for (size_t i = 0; i < p->n_in; ++i) free(p->in_cols[i].name);
    free(p->in_cols);
    if (p->out_staging) {
        for (size_t i = 0; i < p->n_out; ++i) ds_out_free(&p->out_staging[i]);
        free(p->out_staging);
    }
    free(p->pending_set);
    free(p);
}

static const BetlPortDef dpc_inputs[]  = {
    { .name = "in",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "rows to process" },
};
static const BetlPortDef dpc_outputs[] = {
    { .name = "out", .schema_mode = BETL_SCHEMA_DYNAMIC,
      .doc = "rows emitted by ProcessInput (Phase 1a: output_schema-defined)" },
};


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

    { .name               = "dotnet.script",
      .kind               = BETL_KIND_TRANSFORM,
      .config_schema_json = "{}",
      .flags              = 0,
      .inputs             = ds_inputs,
      .input_count        = 1,
      .outputs            = ds_outputs,
      .output_count       = 1,
      .init               = dotnet_script_init,
      .destroy            = dotnet_script_destroy,
      .attach_input       = dotnet_script_attach_input,
      .attach_output      = dotnet_script_attach_output },

    { .name               = "dotnet.pipelinecomponent",
      .kind               = BETL_KIND_TRANSFORM,
      .config_schema_json = "{}",
      .flags              = 0,
      .inputs             = dpc_inputs,
      .input_count        = 1,
      .outputs            = dpc_outputs,
      .output_count       = 1,
      .init               = dotnet_pc_init,
      .destroy            = dotnet_pc_destroy,
      .attach_input       = dotnet_pc_attach_input,
      .attach_output      = dotnet_pc_attach_output },
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
