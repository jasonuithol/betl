/* shell — TASK component: fork + execvp an external program with a
 * literal argv list. No shell expansion, no env tweaks. SSIS Execute
 * Process Task parity at the control-flow layer.
 *
 * Config:
 *   argv:     list of strings, required, argv[0] is the executable
 *   timeout:  optional, e.g. "300s" — process killed (SIGTERM then
 *             SIGKILL) on expiry; the task fails with BETL_ERR_IO
 *
 * Success criterion: child exits with status 0. Non-zero exit, signal
 * death, or timeout all fail the task with a descriptive error. */

#include "runtime/shell.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "betl/provider.h"
#include "runtime/transforms_internal.h"

typedef struct {
    BetlContext *ctx;
    char       **argv;        /* NULL-terminated */
    size_t       argc;
    int          have_timeout;
    int          timeout_s;   /* zero => no timeout */
} ShState;

typedef struct {
    ShState *s;
    int      err;
} ArgvCtx;

static int argv_visit(const char *value, size_t value_len, void *user) {
    ArgvCtx *c = user;
    if (value_len == 0 || value[0] != '"') {
        betl_set_error(c->s->ctx, "shell: `argv:` entries must be strings");
        c->err = 1; return -1;
    }
    char *vbuf = malloc(value_len + 1);
    if (!vbuf) { c->err = 1; return -1; }
    memcpy(vbuf, value, value_len);
    vbuf[value_len] = '\0';
    char *decoded = NULL;
    if (betl_tx_json_decode_str(vbuf, &decoded) != 0 || !decoded) {
        free(vbuf); c->err = 1; return -1;
    }
    free(vbuf);
    char **grow = realloc(c->s->argv,
                          (c->s->argc + 2) * sizeof *grow);
    if (!grow) { free(decoded); c->err = 1; return -1; }
    c->s->argv = grow;
    c->s->argv[c->s->argc++] = decoded;
    c->s->argv[c->s->argc]   = NULL;
    return 0;
}

/* Accept "300", "300s", "10m" — minutes/seconds only. Returns -1 on
 * unparseable; 0 with *out_s on success. Stored as seconds. */
static int parse_timeout(const char *raw, int *out_s) {
    char *end = NULL;
    long v = strtol(raw, &end, 10);
    if (end == raw || v < 0) return -1;
    int mul = 1;
    if (*end == 's' || *end == 'S' || *end == '\0') mul = 1;
    else if (*end == 'm' || *end == 'M')            mul = 60;
    else                                            return -1;
    /* Allow trailing whitespace after the unit. */
    if (mul != 1) ++end;
    while (*end == ' ' || *end == '\t') ++end;
    if (*end != '\0') return -1;
    long long secs = (long long)v * mul;
    if (secs > 86400) return -1;          /* cap at a day */
    *out_s = (int)secs;
    return 0;
}

static int sh_init(BetlContext *ctx, const char *cfg, void **state) {
    ShState *s = calloc(1, sizeof *s);
    if (!s) return BETL_ERR_INTERNAL;
    s->ctx = ctx;
    cfg = cfg ? cfg : "{}";

    const char *argv_pos = betl_tx_json_value_after(cfg, "argv");
    if (!argv_pos || *argv_pos != '[') {
        betl_set_error(ctx, "shell: missing required `argv:` list");
        free(s); return BETL_ERR_INVALID;
    }
    ArgvCtx ac = { .s = s, .err = 0 };
    if (betl_tx_json_walk_array(argv_pos, argv_visit, &ac) != 0 || ac.err) {
        for (size_t i = 0; i < s->argc; ++i) free(s->argv[i]);
        free(s->argv);
        free(s);
        return BETL_ERR_INVALID;
    }
    if (s->argc == 0) {
        betl_set_error(ctx, "shell: `argv:` must contain at least one element "
                            "(the executable)");
        free(s->argv); free(s);
        return BETL_ERR_INVALID;
    }

    char *tout = NULL;
    if (betl_tx_json_string_at(cfg, "timeout", &tout) == 0 && tout) {
        if (parse_timeout(tout, &s->timeout_s) != 0) {
            betl_set_error(ctx,
                "shell: invalid `timeout: %s` (expected e.g. \"60s\", \"5m\")",
                tout);
            free(tout);
            for (size_t i = 0; i < s->argc; ++i) free(s->argv[i]);
            free(s->argv); free(s);
            return BETL_ERR_INVALID;
        }
        s->have_timeout = 1;
        free(tout);
    }

    *state = s;
    return BETL_OK;
}

static void sh_destroy(void *state) {
    if (!state) return;
    ShState *s = state;
    if (s->argv) {
        for (size_t i = 0; i < s->argc; ++i) free(s->argv[i]);
        free(s->argv);
    }
    free(s);
}

static int monotonic_seconds(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int)ts.tv_sec;
}

static int sh_run(void *state) {
    ShState *s = state;

    pid_t pid = fork();
    if (pid < 0) {
        betl_set_error(s->ctx, "shell: fork failed: %s", strerror(errno));
        return BETL_ERR_INTERNAL;
    }
    if (pid == 0) {
        /* Child. New process group so we can SIGTERM the whole tree. */
        setpgid(0, 0);
        execvp(s->argv[0], s->argv);
        /* If exec returns, write a brief tag to stderr and bail. The
         * parent sees the non-zero exit code and reports it. */
        fprintf(stderr, "shell: execvp(%s) failed: %s\n",
                s->argv[0], strerror(errno));
        _exit(127);
    }

    int deadline = s->have_timeout
                       ? monotonic_seconds() + s->timeout_s
                       : 0;
    int killed_by_timeout = 0;
    int status = 0;

    for (;;) {
        pid_t r = waitpid(pid, &status, s->have_timeout ? WNOHANG : 0);
        if (r == pid) break;
        if (r < 0) {
            if (errno == EINTR) continue;
            betl_set_error(s->ctx, "shell: waitpid failed: %s",
                           strerror(errno));
            return BETL_ERR_INTERNAL;
        }
        /* r == 0: still running. We only get here when WNOHANG is set. */
        if (s->have_timeout && monotonic_seconds() >= deadline) {
            /* SIGTERM the process group; escalate after a grace period. */
            kill(-pid, SIGTERM);
            killed_by_timeout = 1;
            int kill_deadline = monotonic_seconds() + 5;
            for (;;) {
                r = waitpid(pid, &status, WNOHANG);
                if (r == pid) goto reaped;
                if (r < 0 && errno != EINTR) break;
                if (monotonic_seconds() >= kill_deadline) {
                    kill(-pid, SIGKILL);
                    waitpid(pid, &status, 0);
                    goto reaped;
                }
                struct timespec sl = { 0, 100 * 1000 * 1000 };  /* 100ms */
                nanosleep(&sl, NULL);
            }
        }
        if (betl_should_cancel(s->ctx)) {
            kill(-pid, SIGTERM);
            killed_by_timeout = 0;
            waitpid(pid, &status, 0);
            betl_set_error(s->ctx, "shell: cancelled by host");
            return BETL_ERR_CANCELLED;
        }
        struct timespec sl = { 0, 50 * 1000 * 1000 };       /* 50ms */
        nanosleep(&sl, NULL);
    }
reaped:
    if (killed_by_timeout) {
        betl_set_error(s->ctx,
            "shell: `%s` exceeded timeout of %ds and was killed",
            s->argv[0], s->timeout_s);
        return BETL_ERR_IO;
    }
    if (WIFSIGNALED(status)) {
        betl_set_error(s->ctx,
            "shell: `%s` killed by signal %d",
            s->argv[0], WTERMSIG(status));
        return BETL_ERR_IO;
    }
    if (!WIFEXITED(status)) {
        betl_set_error(s->ctx,
            "shell: `%s` did not exit normally", s->argv[0]);
        return BETL_ERR_IO;
    }
    int ec = WEXITSTATUS(status);
    if (ec != 0) {
        betl_set_error(s->ctx,
            "shell: `%s` exited with status %d", s->argv[0], ec);
        return BETL_ERR_IO;
    }
    return BETL_OK;
}

static const BetlComponentDef sh_components[] = {
    { .name               = "shell",
      .kind               = BETL_KIND_TASK,
      .config_schema_json = "{}",
      .flags              = 0,
      .init               = sh_init,
      .destroy            = sh_destroy,
      .task_run           = sh_run },
};

static const BetlProvider sh_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-shell",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = sh_components,
    .component_count = sizeof sh_components / sizeof sh_components[0],
};

int betl_register_shell(BetlRegistry *r) {
    return betl_registry_register(r, &sh_provider, "<builtin:shell>");
}
