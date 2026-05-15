/* file.copy / file.move / file.delete TASK components. POSIX-only
 * (open / read / write / rename / unlink). Path handling is verbatim
 * — placeholder substitution happens upstream in the executor.
 *
 * file.copy:   { src: "...", dst: "..." }
 * file.move:   { src: "...", dst: "..." }
 * file.delete: { path: "..." }
 *
 * Behavior:
 *  - copy: overwrites the destination if it exists (matches the SSIS
 *    OverwriteDestinationFile=True default that dtsx2yaml emits as
 *    the no-op shape).
 *  - move: tries rename(2) first; falls back to copy+unlink for
 *    cross-device moves.
 *  - delete: missing path is an error (silent-ok is not the SSIS
 *    default; users who want it should guard with a conditional).
 */

#include "runtime/file_ops.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "betl/provider.h"
#include "runtime/transforms_internal.h"

typedef struct {
    BetlContext *ctx;
    char        *src;       /* file.copy, file.move */
    char        *dst;       /* file.copy, file.move */
    char        *path;      /* file.delete */
} FoState;

static void fo_destroy(void *state) {
    if (!state) return;
    FoState *s = state;
    free(s->src);
    free(s->dst);
    free(s->path);
    free(s);
}

static int require_string(BetlContext *ctx, const char *cfg, const char *key,
                          const char *tag, char **out) {
    if (betl_tx_json_string_at(cfg, key, out) != 0 || !*out) {
        betl_set_error(ctx, "%s: missing required `%s`", tag, key);
        return BETL_ERR_INVALID;
    }
    return BETL_OK;
}

/* ---------------------------------------------------------------- *
 *  Lifecycle (shared init for copy/move, separate for delete)
 * ---------------------------------------------------------------- */

static int copy_or_move_init(BetlContext *ctx, const char *cfg,
                             const char *tag, void **state) {
    FoState *s = calloc(1, sizeof *s);
    if (!s) return BETL_ERR_INTERNAL;
    s->ctx = ctx;
    cfg = cfg ? cfg : "{}";
    int rc = require_string(ctx, cfg, "src", tag, &s->src);
    if (rc != BETL_OK) { fo_destroy(s); return rc; }
    rc = require_string(ctx, cfg, "dst", tag, &s->dst);
    if (rc != BETL_OK) { fo_destroy(s); return rc; }
    *state = s;
    return BETL_OK;
}

static int copy_init(BetlContext *ctx, const char *cfg, void **state) {
    return copy_or_move_init(ctx, cfg, "file.copy", state);
}

static int move_init(BetlContext *ctx, const char *cfg, void **state) {
    return copy_or_move_init(ctx, cfg, "file.move", state);
}

static int delete_init(BetlContext *ctx, const char *cfg, void **state) {
    FoState *s = calloc(1, sizeof *s);
    if (!s) return BETL_ERR_INTERNAL;
    s->ctx = ctx;
    cfg = cfg ? cfg : "{}";
    int rc = require_string(ctx, cfg, "path", "file.delete", &s->path);
    if (rc != BETL_OK) { fo_destroy(s); return rc; }
    *state = s;
    return BETL_OK;
}

/* ---------------------------------------------------------------- *
 *  Runners
 * ---------------------------------------------------------------- */

static int do_copy(BetlContext *ctx, const char *src, const char *dst) {
    int in = open(src, O_RDONLY);
    if (in < 0) {
        betl_set_error(ctx, "file.copy: open(%s): %s", src, strerror(errno));
        return BETL_ERR_IO;
    }
    struct stat st;
    if (fstat(in, &st) != 0) {
        betl_set_error(ctx, "file.copy: fstat(%s): %s", src, strerror(errno));
        close(in);
        return BETL_ERR_IO;
    }
    mode_t mode = (st.st_mode & 0777);
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode ? mode : 0644);
    if (out < 0) {
        betl_set_error(ctx, "file.copy: open(%s): %s", dst, strerror(errno));
        close(in);
        return BETL_ERR_IO;
    }
    char buf[64 * 1024];
    for (;;) {
        ssize_t n = read(in, buf, sizeof buf);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            betl_set_error(ctx, "file.copy: read(%s): %s",
                           src, strerror(errno));
            close(in); close(out);
            unlink(dst);
            return BETL_ERR_IO;
        }
        const char *p = buf;
        ssize_t left = n;
        while (left > 0) {
            ssize_t w = write(out, p, (size_t)left);
            if (w < 0) {
                if (errno == EINTR) continue;
                betl_set_error(ctx, "file.copy: write(%s): %s",
                               dst, strerror(errno));
                close(in); close(out);
                unlink(dst);
                return BETL_ERR_IO;
            }
            p    += w;
            left -= w;
        }
    }
    close(in);
    if (close(out) != 0) {
        betl_set_error(ctx, "file.copy: close(%s): %s", dst, strerror(errno));
        unlink(dst);
        return BETL_ERR_IO;
    }
    return BETL_OK;
}

static int copy_run(void *state) {
    FoState *s = state;
    return do_copy(s->ctx, s->src, s->dst);
}

static int move_run(void *state) {
    FoState *s = state;
    if (rename(s->src, s->dst) == 0) return BETL_OK;
    if (errno != EXDEV) {
        betl_set_error(s->ctx, "file.move: rename(%s -> %s): %s",
                       s->src, s->dst, strerror(errno));
        return BETL_ERR_IO;
    }
    /* Cross-filesystem; copy + unlink. */
    int rc = do_copy(s->ctx, s->src, s->dst);
    if (rc != BETL_OK) return rc;
    if (unlink(s->src) != 0) {
        betl_set_error(s->ctx, "file.move: unlink(%s) after copy: %s",
                       s->src, strerror(errno));
        return BETL_ERR_IO;
    }
    return BETL_OK;
}

static int delete_run(void *state) {
    FoState *s = state;
    if (unlink(s->path) != 0) {
        betl_set_error(s->ctx, "file.delete: unlink(%s): %s",
                       s->path, strerror(errno));
        return BETL_ERR_IO;
    }
    return BETL_OK;
}

/* ---------------------------------------------------------------- *
 *  Provider
 * ---------------------------------------------------------------- */

static const BetlComponentDef fo_components[] = {
    { .name               = "file.copy",
      .kind               = BETL_KIND_TASK,
      .config_schema_json = "{}",
      .flags              = 0,
      .init               = copy_init,
      .destroy            = fo_destroy,
      .task_run           = copy_run },
    { .name               = "file.move",
      .kind               = BETL_KIND_TASK,
      .config_schema_json = "{}",
      .flags              = 0,
      .init               = move_init,
      .destroy            = fo_destroy,
      .task_run           = move_run },
    { .name               = "file.delete",
      .kind               = BETL_KIND_TASK,
      .config_schema_json = "{}",
      .flags              = 0,
      .init               = delete_init,
      .destroy            = fo_destroy,
      .task_run           = delete_run },
};

static const BetlProvider fo_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-file-ops",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = fo_components,
    .component_count = sizeof fo_components / sizeof fo_components[0],
};

int betl_register_file_ops(BetlRegistry *r) {
    return betl_registry_register(r, &fo_provider, "<builtin:file-ops>");
}
