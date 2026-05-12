/* dotnet.task hello-world test.
 *
 * argv[1] = path to betl-dotnet.so.
 *
 * Phase-1 coverage:
 *   1. Compile + run a trivial C# task that calls Log.Info("...") and
 *      verify the message lands in the captured log stream.
 *   2. Compile cache: second run with the same source must reuse the
 *      cached .so (no second `dotnet publish` invocation). We detect
 *      cache reuse by timing — second run is <500ms vs first run's
 *      multi-second AOT compile.
 *   3. VB.NET is rejected with a clear error (phase 2 will wire it).
 *
 * The test is SKIP-coded (rc=77) when the .NET SDK isn't installed —
 * deps/dotnet/ is project-local and not source-controlled. Run
 * scripts/install-dotnet.sh first to enable. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "betl/provider.h"
#include "loader/registry.h"
#include "pipeline/pipeline.h"
#include "runtime/builtins.h"
#include "runtime/context.h"
#include "runtime/exec.h"

#define SKIP_RC 77

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } \
} while (0)

static int write_file(const char *path, const char *contents) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t n = strlen(contents);
    int rc = (fwrite(contents, 1, n, f) == n) ? 0 : -1;
    fclose(f);
    return rc;
}

/* SKIP if .NET SDK isn't reachable. */
static int sdk_available(void) {
    const char *paths[] = {
        "deps/dotnet/dotnet",
        "/workspace/betl/deps/dotnet/dotnet",
        "/opt/projects/betl/deps/dotnet/dotnet",
        NULL
    };
    for (int i = 0; paths[i]; ++i) {
        if (access(paths[i], X_OK) == 0) return 1;
    }
    return 0;
}

/* NativeAOT's link step prefers clang, falls back to gcc if clang
 * isn't on PATH, and links against zlib (-lz). SKIP gracefully if
 * neither linker driver is reachable or libz isn't on the link path. */
static int prog_on_path(const char *name) {
    const char *path = getenv("PATH");
    if (!path) return 0;
    char *copy = strdup(path);
    if (!copy) return 0;
    int found = 0;
    for (char *tok = strtok(copy, ":"); tok; tok = strtok(NULL, ":")) {
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", tok, name);
        if (access(p, X_OK) == 0) { found = 1; break; }
    }
    free(copy);
    return found;
}
static int has_aot_link_toolchain(void) {
    if (!prog_on_path("clang") && !prog_on_path("gcc")) return 0;
    const char *libz[] = {
        "/usr/lib/x86_64-linux-gnu/libz.so",
        "/usr/lib/x86_64-linux-gnu/libz.a",
        "/usr/lib/libz.so",
        NULL
    };
    for (int i = 0; libz[i]; ++i) {
        if (access(libz[i], R_OK) == 0) return 1;
    }
    return 0;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* Run a pipeline YAML, returning ctx's last error in `err`. Captures
 * the log stream into `log_path` if provided. */
static int run_yaml(const char *plugin_path,
                    const char *yaml,
                    const char *log_path,
                    char *err, size_t err_cap) {
    char path[64];
    snprintf(path, sizeof path, "/tmp/betl-test-dotnet-%d.yml", (int)getpid());
    if (write_file(path, yaml) != 0) {
        if (err) snprintf(err, err_cap, "write temp yaml failed");
        return BETL_ERR_IO;
    }
    char load_err[1024] = {0};
    BetlPipeline *p = betl_pipeline_load(path, load_err, sizeof load_err);
    if (!p) {
        if (err) snprintf(err, err_cap, "%s", load_err);
        unlink(path);
        return BETL_ERR_INVALID;
    }

    FILE *log_f = NULL;
    if (log_path) {
        log_f = fopen(log_path, "w+");
        if (!log_f) {
            if (err) snprintf(err, err_cap, "open log file failed");
            betl_pipeline_free(p);
            unlink(path);
            return BETL_ERR_IO;
        }
    }

    BetlContext  *ctx = betl_context_create();
    BetlRegistry *reg = betl_registry_create();
    int rc = BETL_ERR_INTERNAL;
    if (!ctx || !reg) goto cleanup;
    if (log_f) {
        betl_context_set_log_stream(ctx, log_f);
        betl_context_set_min_log_level(ctx, BETL_LOG_TRACE);
    }
    rc = betl_register_builtins(reg);
    if (rc != BETL_OK) {
        if (err) snprintf(err, err_cap, "%s", betl_registry_last_error(reg));
        goto cleanup;
    }
    rc = betl_registry_load(reg, plugin_path);
    if (rc != BETL_OK) {
        if (err) snprintf(err, err_cap, "%s", betl_registry_last_error(reg));
        goto cleanup;
    }
    rc = betl_run(ctx, reg, p);
    if (err) snprintf(err, err_cap, "%s", betl_context_last_error(ctx));

cleanup:
    betl_pipeline_free(p);
    betl_registry_destroy(reg);
    betl_context_destroy(ctx);
    if (log_f) fclose(log_f);
    unlink(path);
    return rc;
}

static int file_contains(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return 0; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return 0; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    int hit = strstr(buf, needle) != NULL;
    free(buf);
    return hit;
}

/* --- Pipelines -------------------------------------------------- */

static const char PL_HELLO_CSHARP[] =
    "betl: 1\n"
    "name: dotnet-task-hello\n"
    "pipeline:\n"
    "  - id: hi\n"
    "    type: dotnet.task\n"
    "    lang: csharp\n"
    "    source: |\n"
    "      public class UserTask : Betl.BetlTask {\n"
    "        public override void Run() {\n"
    "          Betl.Log.Info(\"hello from C# (dotnet.task v0.2)\");\n"
    "        }\n"
    "      }\n";

static const char PL_VBNET_REJECTED[] =
    "betl: 1\n"
    "name: dotnet-task-vbnet-phase2\n"
    "pipeline:\n"
    "  - id: hi\n"
    "    type: dotnet.task\n"
    "    lang: vbnet\n"
    "    source: |\n"
    "      Public Class UserTask : Inherits Betl.BetlTask\n"
    "      End Class\n";

int main(int argc, char **argv) {
    if (!sdk_available()) {
        fprintf(stderr, "[skip] .NET SDK not installed; "
                        "run scripts/install-dotnet.sh\n");
        return SKIP_RC;
    }
    if (!has_aot_link_toolchain()) {
        fprintf(stderr,
            "[skip] NativeAOT host toolchain not available "
            "(install clang + zlib1g-dev)\n");
        return SKIP_RC;
    }
    const char *plugin_path = (argc >= 2) ? argv[1] :
#ifdef BETL_TEST_PLUGIN_PATH
        BETL_TEST_PLUGIN_PATH;
#else
        NULL;
    if (!plugin_path) {
        fprintf(stderr, "usage: %s <path-to-betl-dotnet.so>\n", argv[0]);
        return 2;
    }
#endif

    char err[1024];
    int  rc;

    /* --- 1. Hello-world C# task with log capture. ---------------- */
    char log1[64];
    snprintf(log1, sizeof log1, "/tmp/betl-dotnet-log-%d.txt", (int)getpid());
    double t0 = now_ms();
    err[0] = 0;
    rc = run_yaml(plugin_path, PL_HELLO_CSHARP, log1, err, sizeof err);
    double t1 = now_ms();
    if (rc != BETL_OK) fprintf(stderr, "hello: %s\n", err);
    CHECK(rc == BETL_OK);
    CHECK(file_contains(log1, "hello from C# (dotnet.task v0.2)"));
    fprintf(stderr, "[timing] first run (AOT compile): %.0f ms\n", t1 - t0);

    /* --- 2. Cache hit: rerunning the same source should skip the
     * AOT compile. The dotnet publish step is multi-second; a cache
     * hit is well under 1 second. ---------------------------------- */
    char log2[64];
    snprintf(log2, sizeof log2, "/tmp/betl-dotnet-log2-%d.txt", (int)getpid());
    double c0 = now_ms();
    err[0] = 0;
    rc = run_yaml(plugin_path, PL_HELLO_CSHARP, log2, err, sizeof err);
    double c1 = now_ms();
    if (rc != BETL_OK) fprintf(stderr, "cache-hit: %s\n", err);
    CHECK(rc == BETL_OK);
    CHECK(file_contains(log2, "hello from C# (dotnet.task v0.2)"));
    fprintf(stderr, "[timing] second run (cache hit): %.0f ms\n", c1 - c0);
    CHECK((c1 - c0) < (t1 - t0));     /* cache hit must beat fresh compile */
    CHECK((c1 - c0) < 1000.0);        /* and be under a second */

    /* --- 3. VB.NET is rejected with a clear "phase 2" error. ------ */
    err[0] = 0;
    rc = run_yaml(plugin_path, PL_VBNET_REJECTED, NULL, err, sizeof err);
    CHECK(rc != BETL_OK);
    CHECK(strstr(err, "vbnet") != NULL || strstr(err, "phase 2") != NULL);

    unlink(log1); unlink(log2);

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok: dotnet.task hello-world test passed\n");
    return 0;
}
