/* Async-buffer wrapper around an Arrow stream.
 *
 * Wraps an existing ArrowArrayStream with a producer thread + bounded
 * ring buffer so the consumer can pull from the wrapper while the
 * upstream's get_next runs concurrently on the producer thread. Used
 * by the dataflow executor to overlap I/O between adjacent steps in
 * a pipeline; opt-out via BETL_PARALLEL=off. */

#ifndef BETL_RUNTIME_ASYNC_STREAM_H
#define BETL_RUNTIME_ASYNC_STREAM_H

#include <stddef.h>

#include "betl/provider.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Take ownership of `upstream` (caller's struct is zeroed on success)
 * and populate `out` with a wrapper that runs `upstream.get_next` on
 * a background thread and exposes a bounded ring of `depth` batches
 * to its consumer. `ctx` is polled via betl_should_cancel from the
 * producer.
 *
 * On success returns BETL_OK; on failure (OOM / pthread_*) returns a
 * BETL_ERR_* code, leaves `*upstream` and `*out` zeroed-or-untouched,
 * and the caller still owns the original upstream. */
int betl_async_wrap(struct ArrowArrayStream *upstream,
                    size_t depth,
                    BetlContext *ctx,
                    struct ArrowArrayStream *out);

/* Read environment / sensible defaults to decide whether the executor
 * should wrap edges in parallel mode. Returns 1 (on) by default; 0 only
 * if `BETL_PARALLEL` is set to one of off/0/false/no (case-insensitive).
 * Reads the env once and caches; safe to call repeatedly. */
int betl_pipeline_parallel_enabled(void);

/* Read BETL_PARALLEL_DEPTH (clamped to [1, 64]); default 4. Cached. */
size_t betl_pipeline_parallel_depth(void);

#ifdef __cplusplus
}
#endif

#endif /* BETL_RUNTIME_ASYNC_STREAM_H */
