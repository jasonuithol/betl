/* betl micro-benchmark driver.
 *
 * Runs one named pipeline shape N times, times each iteration with
 * CLOCK_MONOTONIC + getrusage, prints a one-line CSV summary:
 *
 *   shape,mode,iters,rows,wall_min_ms,wall_p50_ms,wall_max_ms,
 *   wall_mean_ms,user_ms,sys_ms,maxrss_kb,rows_per_sec
 *
 * The mode label is informational only — it comes from the env (or
 * the --mode flag for the summary). Parallelism is governed by
 * BETL_PARALLEL / BETL_PARALLEL_DEPTH read once inside async_stream.
 * Run this binary in a subprocess per mode to compare.
 *
 * Shapes:
 *   filter-count    gen_int64(batch_size=1k) → filter(true) → count
 *   map-arith       gen_int64 → ssisexpr (id*3+7 ...) → count
 *   sort            gen_int64 → sort by id desc → count
 *   csv-rt          csv.read → map (project) → csv.write
 *   chain           gen_int64 → 4× ssisexpr → count
 *
 * For csv-rt: a CSV file is generated once before the timed loop so
 * the I/O cost dominates each iteration. The generated file is wiped
 * at exit.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "betl/provider.h"
#include "loader/registry.h"
#include "pipeline/pipeline.h"
#include "runtime/builtins.h"
#include "runtime/context.h"
#include "runtime/exec.h"

/* Plugin loading mirrors cmd_run's autoload. Looks two places:
 *   1. ../providers/<name>/betl-<name>.so relative to the bench binary
 *      (matches the in-tree CMake layout, with bench/ and providers/
 *      siblings under build/).
 *   2. each colon-separated entry in BETL_PROVIDER_DIR.
 * Silently ignores misses — shapes that only need builtins still work. */
static void load_glob(BetlRegistry *reg, const char *pattern) {
    glob_t g;
    if (glob(pattern, 0, NULL, &g) != 0) return;
    for (size_t i = 0; i < g.gl_pathc; ++i) {
        int rc = betl_registry_load(reg, g.gl_pathv[i]);
        if (rc != BETL_OK) {
            fprintf(stderr, "bench: load(%s) failed: %s\n",
                    g.gl_pathv[i], betl_registry_last_error(reg));
        }
    }
    globfree(&g);
}
static void autoload_providers(BetlRegistry *reg) {
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            char pat[4096 + 64];
            snprintf(pat, sizeof pat, "%s/../providers/*/betl-*.so", exe);
            load_glob(reg, pat);
        }
    }
    const char *env = getenv("BETL_PROVIDER_DIR");
    if (env && *env) {
        char buf[4096];
        size_t bl = strlen(env);
        if (bl < sizeof buf) {
            memcpy(buf, env, bl + 1);
            char *tok = buf;
            while (tok && *tok) {
                char *colon = strchr(tok, ':');
                if (colon) *colon = '\0';
                if (*tok) {
                    char pat[4096 + 64];
                    snprintf(pat, sizeof pat, "%s/betl-*.so", tok);
                    load_glob(reg, pat);
                }
                tok = colon ? colon + 1 : NULL;
            }
        }
    }
}

/* ----- timing ---------------------------------------------------- */

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static double tv_ms(struct timeval tv) {
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

/* ----- pipeline runner ------------------------------------------- */

static int write_yaml(const char *path, const char *yaml) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    size_t n = strlen(yaml);
    int rc = fwrite(yaml, 1, n, f) == n ? 0 : -1;
    fclose(f);
    return rc;
}

static int run_yaml(const char *yaml, char *err, size_t err_cap) {
    char path[64];
    snprintf(path, sizeof path, "/tmp/betl-bench-%d.yml", (int)getpid());
    if (write_yaml(path, yaml) != 0) return BETL_ERR_IO;
    char load_err[1024] = {0};
    BetlPipeline *p = betl_pipeline_load(path, load_err, sizeof load_err);
    if (!p) {
        if (err) snprintf(err, err_cap, "%s", load_err);
        unlink(path);
        return BETL_ERR_INVALID;
    }
    BetlContext  *ctx = betl_context_create();
    BetlRegistry *reg = betl_registry_create();
    int rc = BETL_ERR_INTERNAL;
    if (ctx) betl_context_set_min_log_level(ctx, BETL_LOG_WARN);
    if (ctx && reg && betl_register_builtins(reg) == BETL_OK) {
        autoload_providers(reg);
        rc = betl_run(ctx, reg, p);
        if (rc != BETL_OK && err) {
            snprintf(err, err_cap, "%s", betl_context_last_error(ctx));
        }
    }
    betl_pipeline_free(p);
    betl_registry_destroy(reg);
    betl_context_destroy(ctx);
    unlink(path);
    return rc;
}

/* ----- shape builders -------------------------------------------- */

/* shape = "filter-count" */
static void build_filter_count(char *buf, size_t cap,
                               int rows, int batch, const char *csv_in,
                               const char *csv_out) {
    (void)csv_in; (void)csv_out;
    snprintf(buf, cap,
        "betl: 1\n"
        "name: bench-filter-count\n"
        "pipeline:\n"
        "  - id: stage\n"
        "    type: dataflow\n"
        "    steps:\n"
        "      - id: source\n"
        "        type: betl.gen_int64\n"
        "        row_count: %d\n"
        "        batch_size: %d\n"
        "      - id: keep\n"
        "        type: filter\n"
        "        from: source\n"
        "        where: { lang: literal, value: \"true\" }\n"
        "      - id: sink\n"
        "        type: betl.count_rows\n"
        "        from: keep\n"
        "        expect: %d\n",
        rows, batch, rows);
}

/* shape = "map-arith" — per-row arithmetic via ssisexpr add column */
static void build_map_arith(char *buf, size_t cap,
                            int rows, int batch, const char *csv_in,
                            const char *csv_out) {
    (void)csv_in; (void)csv_out;
    snprintf(buf, cap,
        "betl: 1\n"
        "name: bench-map-arith\n"
        "pipeline:\n"
        "  - id: stage\n"
        "    type: dataflow\n"
        "    steps:\n"
        "      - id: source\n"
        "        type: betl.gen_int64\n"
        "        row_count: %d\n"
        "        batch_size: %d\n"
        "      - id: m\n"
        "        type: map\n"
        "        from: source\n"
        "        add:\n"
        "          v:\n"
        "            lang: ssisexpr\n"
        "            expr: \"[id] * 3 + 7\"\n"
        "            type: l\n"
        "      - id: sink\n"
        "        type: betl.count_rows\n"
        "        from: m\n"
        "        expect: %d\n",
        rows, batch, rows);
}

/* shape = "sort" — materializing transform */
static void build_sort(char *buf, size_t cap,
                       int rows, int batch, const char *csv_in,
                       const char *csv_out) {
    (void)csv_in; (void)csv_out;
    snprintf(buf, cap,
        "betl: 1\n"
        "name: bench-sort\n"
        "pipeline:\n"
        "  - id: stage\n"
        "    type: dataflow\n"
        "    steps:\n"
        "      - id: source\n"
        "        type: betl.gen_int64\n"
        "        row_count: %d\n"
        "        batch_size: %d\n"
        "      - id: s\n"
        "        type: sort\n"
        "        from: source\n"
        "        by:\n"
        "          - { col: id, dir: desc }\n"
        "      - id: sink\n"
        "        type: betl.count_rows\n"
        "        from: s\n"
        "        expect: %d\n",
        rows, batch, rows);
}

/* shape = "chain" — four map stages back-to-back */
static void build_chain(char *buf, size_t cap,
                        int rows, int batch, const char *csv_in,
                        const char *csv_out) {
    (void)csv_in; (void)csv_out;
    snprintf(buf, cap,
        "betl: 1\n"
        "name: bench-chain\n"
        "pipeline:\n"
        "  - id: stage\n"
        "    type: dataflow\n"
        "    steps:\n"
        "      - id: source\n"
        "        type: betl.gen_int64\n"
        "        row_count: %d\n"
        "        batch_size: %d\n"
        "      - id: m1\n"
        "        type: map\n"
        "        from: source\n"
        "        add:\n"
        "          a: { lang: ssisexpr, expr: \"[id] + 1\", type: l }\n"
        "      - id: m2\n"
        "        type: map\n"
        "        from: m1\n"
        "        add:\n"
        "          b: { lang: ssisexpr, expr: \"[a] * 2\", type: l }\n"
        "      - id: m3\n"
        "        type: map\n"
        "        from: m2\n"
        "        add:\n"
        "          c: { lang: ssisexpr, expr: \"[b] - [id]\", type: l }\n"
        "      - id: m4\n"
        "        type: map\n"
        "        from: m3\n"
        "        add:\n"
        "          d: { lang: ssisexpr, expr: \"[c] + 5\", type: l }\n"
        "      - id: sink\n"
        "        type: betl.count_rows\n"
        "        from: m4\n"
        "        expect: %d\n",
        rows, batch, rows);
}

/* shape = "csv-rt" — csv.read → map → csv.write */
static void build_csv_rt(char *buf, size_t cap,
                         int rows, int batch, const char *csv_in,
                         const char *csv_out) {
    (void)rows;
    snprintf(buf, cap,
        "betl: 1\n"
        "name: bench-csv-rt\n"
        "pipeline:\n"
        "  - id: stage\n"
        "    type: dataflow\n"
        "    steps:\n"
        "      - id: source\n"
        "        type: csv.read\n"
        "        path: %s\n"
        "        batch_size: %d\n"
        "        schema:\n"
        "          columns:\n"
        "            - { name: id,   type: int64 }\n"
        "            - { name: data, type: utf8 }\n"
        "      - id: m\n"
        "        type: map\n"
        "        from: source\n"
        "        add:\n"
        "          v: { lang: ssisexpr, expr: \"[id] * 2\", type: l }\n"
        "      - id: sink\n"
        "        type: csv.write\n"
        "        from: m\n"
        "        path: %s\n",
        csv_in, batch, csv_out);
}

/* ----- shape table ----------------------------------------------- */

typedef void (*build_fn)(char *, size_t, int, int, const char *, const char *);

typedef struct {
    const char *name;
    build_fn    build;
    int         needs_csv_in;
    int         writes_csv_out;
    const char *description;
} Shape;

static const Shape SHAPES[] = {
    { "filter-count", build_filter_count, 0, 0,
      "gen → filter(true) → count" },
    { "map-arith",    build_map_arith,    0, 0,
      "gen → ssisexpr arithmetic → count" },
    { "sort",         build_sort,         0, 0,
      "gen → sort desc → count (materializes)" },
    { "chain",        build_chain,        0, 0,
      "gen → 4× ssisexpr map → count" },
    { "csv-rt",       build_csv_rt,       1, 1,
      "csv.read → ssisexpr map → csv.write" },
};
static const size_t N_SHAPES = sizeof SHAPES / sizeof SHAPES[0];

/* ----- CSV prep -------------------------------------------------- */

/* Build a CSV with `rows` rows of (int64 id, utf8 data) — 100-byte
 * filler per row gives ~110 bytes/row → ~110MB for 1M rows. */
static int prepare_csv(const char *path, int rows) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs("id,data\n", f);
    static const char filler[] =
        "the.quick.brown.fox.jumps.over.the.lazy.dog."
        "0123456789abcdef0123456789abcdef0123456789abcdef00";
    for (int i = 0; i < rows; ++i) {
        fprintf(f, "%d,%s\n", i, filler);
    }
    fclose(f);
    return 0;
}

/* ----- stats ----------------------------------------------------- */

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
            "usage: %s <shape> <iterations> [--rows N] [--batch N] [--mode LABEL]\n"
            "shapes:\n", argv[0]);
        for (size_t i = 0; i < N_SHAPES; ++i) {
            fprintf(stderr, "  %-13s %s\n", SHAPES[i].name, SHAPES[i].description);
        }
        return 2;
    }
    const char *shape_name = argv[1];
    int iters = atoi(argv[2]);
    if (iters < 1) iters = 5;

    int rows = 1000000;
    int batch = 4096;
    const char *mode_label = getenv("BETL_PARALLEL");
    if (!mode_label || !*mode_label) mode_label = "default";
    for (int i = 3; i < argc; ++i) {
        if (!strcmp(argv[i], "--rows")  && i + 1 < argc) rows  = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--batch") && i + 1 < argc) batch = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mode")  && i + 1 < argc) mode_label = argv[++i];
    }

    const Shape *sh = NULL;
    for (size_t i = 0; i < N_SHAPES; ++i) {
        if (!strcmp(SHAPES[i].name, shape_name)) { sh = &SHAPES[i]; break; }
    }
    if (!sh) {
        fprintf(stderr, "unknown shape '%s'\n", shape_name);
        return 2;
    }

    char csv_in[128] = {0}, csv_out[128] = {0};
    snprintf(csv_in,  sizeof csv_in,  "/tmp/betl-bench-%d-in.csv",  (int)getpid());
    snprintf(csv_out, sizeof csv_out, "/tmp/betl-bench-%d-out.csv", (int)getpid());
    if (sh->needs_csv_in) {
        if (prepare_csv(csv_in, rows) != 0) {
            fprintf(stderr, "csv prep failed\n");
            return 1;
        }
    }

    char yaml[4096];
    sh->build(yaml, sizeof yaml, rows, batch, csv_in, csv_out);

    double *wall = calloc((size_t)iters, sizeof *wall);
    if (!wall) return 1;
    double total_user_ms = 0.0, total_sys_ms = 0.0;
    long   max_rss_kb = 0;

    /* Warm-up pass — first run sees disk cache misses + lazy alloc. */
    {
        char err[1024] = {0};
        int rc = run_yaml(yaml, err, sizeof err);
        if (rc != BETL_OK) {
            fprintf(stderr, "warmup failed: %s\n", err);
            free(wall);
            if (sh->needs_csv_in) unlink(csv_in);
            if (sh->writes_csv_out) unlink(csv_out);
            return 1;
        }
    }

    for (int i = 0; i < iters; ++i) {
        struct rusage ru_before, ru_after;
        getrusage(RUSAGE_SELF, &ru_before);
        double t0 = now_ms();
        char err[1024] = {0};
        int rc = run_yaml(yaml, err, sizeof err);
        double t1 = now_ms();
        getrusage(RUSAGE_SELF, &ru_after);
        if (rc != BETL_OK) {
            fprintf(stderr, "iter %d failed: %s\n", i, err);
            free(wall);
            if (sh->needs_csv_in) unlink(csv_in);
            if (sh->writes_csv_out) unlink(csv_out);
            return 1;
        }
        wall[i] = t1 - t0;
        total_user_ms += tv_ms(ru_after.ru_utime) - tv_ms(ru_before.ru_utime);
        total_sys_ms  += tv_ms(ru_after.ru_stime) - tv_ms(ru_before.ru_stime);
        if (ru_after.ru_maxrss > max_rss_kb) max_rss_kb = ru_after.ru_maxrss;
    }
    if (sh->needs_csv_in)  unlink(csv_in);
    if (sh->writes_csv_out) unlink(csv_out);

    qsort(wall, (size_t)iters, sizeof *wall, cmp_double);
    double w_min = wall[0];
    double w_max = wall[iters - 1];
    double w_p50 = wall[iters / 2];
    double w_sum = 0.0;
    for (int i = 0; i < iters; ++i) w_sum += wall[i];
    double w_mean = w_sum / (double)iters;
    double user_mean = total_user_ms / (double)iters;
    double sys_mean  = total_sys_ms  / (double)iters;
    double rps = (w_min > 0.0) ? (double)rows / (w_min / 1000.0) : 0.0;

    printf("%s,%s,%d,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%ld,%.0f\n",
           sh->name, mode_label, iters, rows,
           w_min, w_p50, w_max, w_mean,
           user_mean, sys_mean, max_rss_kb, rps);

    free(wall);
    return 0;
}
