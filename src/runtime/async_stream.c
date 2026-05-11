/* Async-buffer wrapper around ArrowArrayStream — see header for the
 * design overview.
 *
 * Threading model:
 *   - One producer thread per wrapped edge. The thread spawns lazily
 *     on the first consumer get_next() call (so we don't create a
 *     thread for streams that never get pulled, and so the consumer's
 *     synchronous get_schema() call can run before the thread starts).
 *   - A single mutex protects the ring + flags. Two condvars: not_empty
 *     wakes the consumer; not_full wakes the producer. */

#include "runtime/async_stream.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "betl/provider.h"

typedef struct {
    pthread_t        thread;
    pthread_mutex_t  mu;
    pthread_cond_t   not_empty;
    pthread_cond_t   not_full;

    struct ArrowArray *ring;
    size_t  cap;
    size_t  head;
    size_t  n;

    int     started;          /* producer thread spawned */
    int     eof;              /* upstream returned EOF */
    int     stop;             /* consumer requested teardown */
    int     err_set;          /* upstream errored — see err_msg */
    char    err_msg[512];

    struct ArrowArrayStream upstream;     /* we own */
    BetlContext            *ctx;
} AsyncStream;

/* ============================================================== *
 *  Configuration                                                   *
 * ============================================================== */

static int  s_parallel_decided = 0;
static int  s_parallel_on      = 1;       /* default ON */
static size_t s_parallel_depth = 4;

static int parse_bool_off(const char *v) {
    if (!v || !*v) return 0;
    char buf[16];
    size_t i = 0;
    for (; v[i] && i + 1 < sizeof buf; ++i) {
        buf[i] = (char)tolower((unsigned char)v[i]);
    }
    buf[i] = '\0';
    return strcmp(buf, "off")   == 0 || strcmp(buf, "0")     == 0
        || strcmp(buf, "false") == 0 || strcmp(buf, "no")    == 0;
}

int betl_pipeline_parallel_enabled(void) {
    if (!s_parallel_decided) {
        const char *v = getenv("BETL_PARALLEL");
        s_parallel_on = !parse_bool_off(v);
        const char *d = getenv("BETL_PARALLEL_DEPTH");
        if (d && *d) {
            char *end = NULL;
            long n = strtol(d, &end, 10);
            if (end != d && *end == '\0' && n >= 1 && n <= 64) {
                s_parallel_depth = (size_t)n;
            }
        }
        s_parallel_decided = 1;
    }
    return s_parallel_on;
}

size_t betl_pipeline_parallel_depth(void) {
    (void)betl_pipeline_parallel_enabled();   /* ensure cache populated */
    return s_parallel_depth;
}

/* ============================================================== *
 *  Producer                                                        *
 * ============================================================== */

static void *producer_loop(void *arg) {
    AsyncStream *q = arg;
    for (;;) {
        /* Wait for room in the ring (or for stop). */
        pthread_mutex_lock(&q->mu);
        while (!q->stop && q->n == q->cap) {
            pthread_cond_wait(&q->not_full, &q->mu);
        }
        int stopping = q->stop;
        pthread_mutex_unlock(&q->mu);
        if (stopping) break;

        /* Cooperative cancel from the host. */
        if (betl_should_cancel(q->ctx)) {
            pthread_mutex_lock(&q->mu);
            q->stop = 1;
            pthread_cond_broadcast(&q->not_empty);
            pthread_mutex_unlock(&q->mu);
            break;
        }

        /* Pull one batch from upstream. This may block on I/O — by
         * design; the consumer can keep working from the queue while
         * we wait. */
        struct ArrowArray batch = {0};
        int rc = q->upstream.get_next(&q->upstream, &batch);
        if (rc != 0) {
            const char *e = q->upstream.get_last_error
                ? q->upstream.get_last_error(&q->upstream) : NULL;
            pthread_mutex_lock(&q->mu);
            q->err_set = 1;
            snprintf(q->err_msg, sizeof q->err_msg, "%s",
                     e && *e ? e : "upstream get_next failed");
            pthread_cond_broadcast(&q->not_empty);
            pthread_mutex_unlock(&q->mu);
            break;
        }
        if (!batch.release) {
            /* Clean EOF — signal and exit. */
            pthread_mutex_lock(&q->mu);
            q->eof = 1;
            pthread_cond_broadcast(&q->not_empty);
            pthread_mutex_unlock(&q->mu);
            break;
        }

        /* Push to the ring, or discard if the consumer asked us to stop
         * while we were mid-get_next. */
        pthread_mutex_lock(&q->mu);
        if (q->stop) {
            pthread_mutex_unlock(&q->mu);
            batch.release(&batch);
            break;
        }
        size_t tail = (q->head + q->n) % q->cap;
        q->ring[tail] = batch;
        ++q->n;
        pthread_cond_signal(&q->not_empty);
        pthread_mutex_unlock(&q->mu);
    }
    return NULL;
}

/* ============================================================== *
 *  Stream interface (consumer-side)                                *
 * ============================================================== */

static int async_get_schema(struct ArrowArrayStream *st,
                            struct ArrowSchema *out) {
    AsyncStream *q = st->private_data;
    if (!q) return EINVAL;
    /* Forward synchronously. The producer is not yet started (or only
     * uses get_next), so this won't race. */
    return q->upstream.get_schema(&q->upstream, out);
}

static int async_get_next(struct ArrowArrayStream *st,
                          struct ArrowArray *out) {
    AsyncStream *q = st->private_data;
    memset(out, 0, sizeof *out);
    if (!q) return EINVAL;

    /* Lazy producer-thread spawn on first pull. */
    if (!q->started) {
        if (pthread_create(&q->thread, NULL, producer_loop, q) != 0) {
            return EIO;
        }
        q->started = 1;
    }

    pthread_mutex_lock(&q->mu);
    while (q->n == 0 && !q->eof && !q->err_set) {
        pthread_cond_wait(&q->not_empty, &q->mu);
    }
    if (q->n > 0) {
        *out = q->ring[q->head];
        memset(&q->ring[q->head], 0, sizeof q->ring[q->head]);
        q->head = (q->head + 1) % q->cap;
        --q->n;
        pthread_cond_signal(&q->not_full);
        pthread_mutex_unlock(&q->mu);
        return 0;
    }
    int err = q->err_set;
    pthread_mutex_unlock(&q->mu);
    /* Either EOF (out has release=NULL → end-of-stream sentinel) or
     * an upstream error (caller will read async_get_last_error). */
    return err ? EIO : 0;
}

static const char *async_get_last_error(struct ArrowArrayStream *st) {
    AsyncStream *q = st->private_data;
    if (!q) return NULL;
    /* err_msg is written exactly once by the producer (under the lock,
     * before the consumer is notified). The consumer reads it after
     * observing err_set under the lock too. After err_set, the producer
     * never touches err_msg again. So a relaxed read here is fine. */
    if (q->err_set) return q->err_msg;
    /* If the producer hasn't started yet, errors can only have come
     * from a synchronous upstream call — typically get_schema fails
     * before any pull. Forward to the upstream's get_last_error so
     * the original message reaches the consumer. After the producer
     * has spawned, only it touches the upstream and errors flow via
     * err_set instead. */
    if (!q->started && q->upstream.get_last_error) {
        return q->upstream.get_last_error(&q->upstream);
    }
    return NULL;
}

static void async_release(struct ArrowArrayStream *st) {
    AsyncStream *q = st->private_data;
    if (!q) { st->release = NULL; return; }

    if (q->started) {
        pthread_mutex_lock(&q->mu);
        q->stop = 1;
        pthread_cond_broadcast(&q->not_full);
        pthread_cond_broadcast(&q->not_empty);
        pthread_mutex_unlock(&q->mu);
        pthread_join(q->thread, NULL);
    }

    /* Drain any leftover batches in the ring (consumer abandoned them). */
    while (q->n > 0) {
        struct ArrowArray *b = &q->ring[q->head];
        if (b->release) b->release(b);
        memset(b, 0, sizeof *b);
        q->head = (q->head + 1) % q->cap;
        --q->n;
    }
    free(q->ring);

    if (q->upstream.release) q->upstream.release(&q->upstream);

    pthread_mutex_destroy(&q->mu);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
    free(q);

    st->private_data = NULL;
    st->release      = NULL;
}

/* ============================================================== *
 *  Constructor                                                     *
 * ============================================================== */

int betl_async_wrap(struct ArrowArrayStream *upstream,
                    size_t depth,
                    BetlContext *ctx,
                    struct ArrowArrayStream *out) {
    if (!upstream || !out) return BETL_ERR_INVALID;
    if (depth < 1) depth = 1;
    if (depth > 64) depth = 64;

    AsyncStream *q = calloc(1, sizeof *q);
    if (!q) return BETL_ERR_INTERNAL;
    q->ring = calloc(depth, sizeof *q->ring);
    if (!q->ring) { free(q); return BETL_ERR_INTERNAL; }
    q->cap = depth;
    q->ctx = ctx;

    if (pthread_mutex_init(&q->mu, NULL) != 0) {
        free(q->ring); free(q); return BETL_ERR_INTERNAL;
    }
    if (pthread_cond_init(&q->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&q->mu);
        free(q->ring); free(q); return BETL_ERR_INTERNAL;
    }
    if (pthread_cond_init(&q->not_full, NULL) != 0) {
        pthread_cond_destroy(&q->not_empty);
        pthread_mutex_destroy(&q->mu);
        free(q->ring); free(q); return BETL_ERR_INTERNAL;
    }

    /* Take ownership of the upstream stream. */
    q->upstream = *upstream;
    memset(upstream, 0, sizeof *upstream);

    out->get_schema     = async_get_schema;
    out->get_next       = async_get_next;
    out->get_last_error = async_get_last_error;
    out->release        = async_release;
    out->private_data   = q;
    return BETL_OK;
}
