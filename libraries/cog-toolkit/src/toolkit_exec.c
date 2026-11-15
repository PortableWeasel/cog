/**
 * @file toolkit_exec.c
 * @brief Implementation of the tool execution dispatcher declared in
 *        cog/toolkit_exec.h.
 *
 * The dispatcher resolves tools by name in the static const tool table and
 * invokes their cog_tool_fn implementations.  Single calls write into a
 * caller-provided result value.  cog_toolkit_execute_all writes into an
 * opaque result collection.
 *
 * Batch execution has a caller-thread fast path for max_parallel == 1 (and
 * for at most one call).  Otherwise a bounded set of POSIX worker threads
 * claims call indices through a mutex-protected work index.  Each worker
 * writes only the result slots belonging to the indices it claimed.  Every
 * launched worker is joined before temporary storage is released or the
 * collection is assembled on the caller thread.
 *
 * Timeout handling remains advisory and enforced at invocation boundaries.
 * Cancellation remains cooperative and is observed before and after each
 * tool invocation; tools are never preempted.
 *
 * The module holds no global mutable state.
 *
 * Ticket: 8d52c1a (Stage 12.4b — Implement toolkit), revision for review
 * 9a39e07 and design 3614303.
 */

#define _POSIX_C_SOURCE 200809L

#include "cog/toolkit_exec.h"
#include "toolkit_internal.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Dispatcher default per-call timeout when neither the call nor the
 * options specify one.  60 seconds is a sensible default for a tool call. */
#define COG_TOOLKIT_DEFAULT_TIMEOUT_MS 60000U

/* Initial output buffer capacity for a tool invoked through the
 * collection path.  The buffer grows on COG_ERROR_OVERFLOW. */
#define COG_TOOLKIT_INITIAL_CAP 256U

/** Monotonic time in milliseconds (CLOCK_MONOTONIC). */
static uint64_t now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0U;
    }
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

/**
 * @brief Invoke a tool, growing its result buffer on overflow.
 *
 * out_buf and out_cap belong exclusively to the claimed call.  If out_buf
 * is NULL an initial buffer is allocated.  On COG_ERROR_OVERFLOW the buffer
 * grows to the reported size and the tool is invoked again.  The caller of
 * this helper owns the returned buffer.
 */
static cog_result_t invoke_tool(cog_core_t *core,
                                const cog_tool_t *tool,
                                const char *request,
                                size_t request_len,
                                char **out_buf,
                                size_t *out_cap,
                                size_t *out_len)
{
    char *buf = *out_buf;
    size_t cap = *out_cap;
    cog_result_t r;

    if (buf == NULL) {
        cap = COG_TOOLKIT_INITIAL_CAP;
        buf = (char *)malloc(cap);
        if (buf == NULL) {
            return COG_ERROR_NOMEM;
        }
        *out_buf = buf;
        *out_cap = cap;
    }

    for (;;) {
        size_t need = 0U;

        r = tool->handler(core, request, request_len, buf, cap, &need);
        if (r == COG_ERROR_OVERFLOW && need > cap) {
            char *nbuf = (char *)realloc(buf, need + 1U);
            if (nbuf == NULL) {
                /* realloc leaves buf intact on failure. */
                return COG_ERROR_NOMEM;
            }
            buf = nbuf;
            cap = need + 1U;
            *out_buf = buf;
            *out_cap = cap;
            continue;
        }
        break;
    }

    *out_buf = buf;
    *out_cap = cap;
    if (r == COG_OK) {
        *out_len = strlen(buf);
    }
    return r;
}

/** Allocate a human-readable error message for a failed call. */
static char *make_error_message(const cog_toolkit_call_t *call,
                                cog_result_t status,
                                size_t *out_len)
{
    const char *name = cog_toolkit_call_name(call);
    const char *base;
    size_t base_len;
    size_t name_len = name != NULL ? strlen(name) : 0U;
    size_t len;
    char *msg;

    switch (status) {
    case COG_ERROR_NOT_FOUND:
        base = "tool not found: ";
        break;
    case COG_ERROR_ABORTED:
        base = "call cancelled: ";
        break;
    case COG_ERROR_TIMEOUT:
        base = "call timed out: ";
        break;
    case COG_ERROR_INVALID_ARG:
        base = "invalid argument: ";
        break;
    case COG_ERROR_NOMEM:
        base = "out of memory: ";
        break;
    case COG_ERROR_OVERFLOW:
        base = "result overflow: ";
        break;
    default:
        base = "tool failed: ";
        break;
    }
    base_len = strlen(base);
    len = base_len + name_len;

    msg = (char *)malloc(len + 1U);
    if (msg == NULL) {
        *out_len = 0U;
        return NULL;
    }
    (void)memcpy(msg, base, base_len);
    if (name_len > 0U) {
        (void)memcpy(msg + base_len, name, name_len);
    }
    msg[len] = '\0';
    *out_len = len;
    return msg;
}

/**
 * State shared by a batch and all its workers.
 *
 * next_index, stop, first_error_index and first_error_status are protected
 * by mutex.  All other pointer fields are initialised before workers launch
 * and are read-only thereafter.  Each result array element is written only
 * by the worker that claimed that element's index.
 */
typedef struct toolkit_batch {
    cog_core_t *core;
    const cog_tool_table_t *table;
    cog_toolkit_call_t *const *calls;
    size_t call_count;
    uint32_t default_timeout_ms;
    bool stop_on_error;

    pthread_mutex_t mutex;
    size_t next_index;
    bool stop;
    size_t first_error_index;
    cog_result_t first_error_status;

    char **bufs;
    size_t *caps;
    size_t *lens;
    cog_result_t *statuses;
    char **errors;
    size_t *error_lens;
} toolkit_batch_t;

/** Run one claimed call, writing only that call's preallocated slots. */
static void run_claimed_call(toolkit_batch_t *batch, size_t index)
{
    cog_toolkit_call_t *call = batch->calls[index];
    const cog_tool_t *tool = NULL;
    cog_result_t r;
    uint32_t timeout_ms;
    uint64_t deadline;
    size_t len = 0U;
    const char *request;
    size_t request_len;

    batch->statuses[index] = COG_OK;

    if (cog_toolkit_call_is_cancelled(call)) {
        batch->statuses[index] = COG_ERROR_ABORTED;
        batch->errors[index] = make_error_message(
            call, COG_ERROR_ABORTED, &batch->error_lens[index]);
        return;
    }

    r = cog_tool_table_lookup(batch->table, cog_toolkit_call_name(call),
                              &tool);
    if (r != COG_OK) {
        batch->statuses[index] = r;
        batch->errors[index] = make_error_message(
            call, r, &batch->error_lens[index]);
        return;
    }

    timeout_ms = cog_toolkit_call_timeout_ms(call);
    if (timeout_ms == 0U) {
        timeout_ms = batch->default_timeout_ms;
    }
    deadline = now_ms() + (uint64_t)timeout_ms;

    request = cog_toolkit_call_request(call, &request_len);
    r = invoke_tool(batch->core, tool, request, request_len,
                    &batch->bufs[index], &batch->caps[index], &len);
    if (r != COG_OK) {
        batch->statuses[index] = r;
        batch->errors[index] = make_error_message(
            call, r, &batch->error_lens[index]);
        return;
    }
    if (now_ms() >= deadline) {
        batch->statuses[index] = COG_ERROR_TIMEOUT;
        batch->errors[index] = make_error_message(
            call, COG_ERROR_TIMEOUT, &batch->error_lens[index]);
        return;
    }
    if (cog_toolkit_call_is_cancelled(call)) {
        batch->statuses[index] = COG_ERROR_ABORTED;
        batch->errors[index] = make_error_message(
            call, COG_ERROR_ABORTED, &batch->error_lens[index]);
        return;
    }
    batch->lens[index] = len;
}

/** Claim and run calls until the batch is empty or stopped. */
static void *batch_worker(void *arg)
{
    toolkit_batch_t *batch = (toolkit_batch_t *)arg;

    for (;;) {
        size_t index;

        (void)pthread_mutex_lock(&batch->mutex);
        if (batch->stop || batch->next_index >= batch->call_count) {
            (void)pthread_mutex_unlock(&batch->mutex);
            break;
        }
        index = batch->next_index;
        batch->next_index++;
        (void)pthread_mutex_unlock(&batch->mutex);

        run_claimed_call(batch, index);

        if (batch->stop_on_error && batch->statuses[index] != COG_OK) {
            (void)pthread_mutex_lock(&batch->mutex);
            if (index < batch->first_error_index) {
                batch->first_error_index = index;
                batch->first_error_status = batch->statuses[index];
            }
            batch->stop = true;
            (void)pthread_mutex_unlock(&batch->mutex);
        }
    }
    return NULL;
}

/** Ask all workers to stop claiming new work (running tools still finish). */
static void batch_request_stop(toolkit_batch_t *batch)
{
    (void)pthread_mutex_lock(&batch->mutex);
    batch->stop = true;
    (void)pthread_mutex_unlock(&batch->mutex);
}

cog_result_t cog_toolkit_execute(cog_core_t *core,
                                 const cog_tool_table_t *table,
                                 cog_toolkit_call_t *call,
                                 cog_toolkit_result_t *result,
                                 size_t result_capacity)
{
    const cog_tool_t *tool = NULL;
    cog_result_t r;
    size_t len = 0U;
    const char *request;
    size_t request_len;

    if (core == NULL || table == NULL || call == NULL || result == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (result->output == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (result_capacity == 0U) {
        return COG_ERROR_INVALID_ARG;
    }

    result->status = COG_OK;
    result->output_len = 0U;
    result->error_message = NULL;

    if (cog_toolkit_call_is_cancelled(call)) {
        result->status = COG_ERROR_ABORTED;
        return COG_ERROR_ABORTED;
    }

    r = cog_tool_table_lookup(table, cog_toolkit_call_name(call), &tool);
    if (r != COG_OK) {
        result->status = r;
        return r;
    }

    /* Single invocation writes directly into the caller-provided buffer;
     * no growth is performed here (the caller owns the buffer). */
    request = cog_toolkit_call_request(call, &request_len);
    r = tool->handler(core, request, request_len,
                      result->output, result_capacity, &len);
    if (r != COG_OK) {
        result->status = r;
        return r;
    }
    if (cog_toolkit_call_is_cancelled(call)) {
        result->status = COG_ERROR_ABORTED;
        return COG_ERROR_ABORTED;
    }

    result->status = COG_OK;
    result->output_len = len;
    return COG_OK;
}

cog_result_t cog_toolkit_execute_all(cog_core_t *core,
                                     const cog_tool_table_t *table,
                                     cog_toolkit_call_t *const *calls,
                                     size_t call_count,
                                     const cog_toolkit_exec_opts_t *opts,
                                     cog_toolkit_result_collection_t **out_results)
{
    cog_toolkit_exec_opts_t local_opts;
    size_t max_parallel;
    toolkit_batch_t batch;
    cog_result_t r = COG_OK;
    size_t i;
    cog_toolkit_result_collection_t *coll = NULL;
    size_t total_str = 0U;
    char *sp;

    if (core == NULL || table == NULL || calls == NULL || out_results == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    *out_results = NULL;

    for (i = 0U; i < call_count; i++) {
        if (calls[i] == NULL) {
            return COG_ERROR_INVALID_ARG;
        }
    }

    if (opts != NULL) {
        local_opts = *opts;
    } else {
        local_opts.timeout_ms = 0U;
        local_opts.max_parallel = 0U;
        local_opts.stop_on_error = false;
    }

    (void)memset(&batch, 0, sizeof(batch));
    batch.core = core;
    batch.table = table;
    batch.calls = calls;
    batch.call_count = call_count;
    batch.default_timeout_ms = local_opts.timeout_ms != 0U
                                  ? local_opts.timeout_ms
                                  : COG_TOOLKIT_DEFAULT_TIMEOUT_MS;
    batch.stop_on_error = local_opts.stop_on_error;
    batch.first_error_index = call_count;
    batch.first_error_status = COG_OK;

    if (call_count > 0U) {
        batch.bufs = (char **)calloc(call_count, sizeof(char *));
        batch.caps = (size_t *)calloc(call_count, sizeof(size_t));
        batch.lens = (size_t *)calloc(call_count, sizeof(size_t));
        batch.statuses = (cog_result_t *)calloc(call_count,
                                               sizeof(cog_result_t));
        batch.errors = (char **)calloc(call_count, sizeof(char *));
        batch.error_lens = (size_t *)calloc(call_count, sizeof(size_t));
        if (batch.bufs == NULL || batch.caps == NULL || batch.lens == NULL ||
            batch.statuses == NULL || batch.errors == NULL ||
            batch.error_lens == NULL) {
            r = COG_ERROR_NOMEM;
            goto cleanup;
        }
    }

    max_parallel = local_opts.max_parallel != 0U
                       ? local_opts.max_parallel
                       : call_count;

    /* Caller-thread fast path.  No mutex or worker state is needed. */
    if (max_parallel <= 1U || call_count <= 1U) {
        for (i = 0U; i < call_count; i++) {
            run_claimed_call(&batch, i);
            if (batch.stop_on_error && batch.statuses[i] != COG_OK) {
                batch.first_error_index = i;
                batch.first_error_status = batch.statuses[i];
                batch.stop = true;
                break;
            }
        }
    } else {
        const size_t worker_count = max_parallel < call_count
                                        ? max_parallel
                                        : call_count;
        pthread_t *threads = (pthread_t *)calloc(worker_count,
                                                sizeof(pthread_t));
        size_t created = 0U;
        bool create_failed = false;
        bool join_failed = false;

        if (threads == NULL) {
            r = COG_ERROR_NOMEM;
            goto cleanup;
        }
        if (pthread_mutex_init(&batch.mutex, NULL) != 0) {
            free(threads);
            r = COG_ERROR_NOMEM;
            goto cleanup;
        }

        for (i = 0U; i < worker_count; i++) {
            int create_status;

            /* If another worker has already failed, do not create more
             * workers just to have them immediately exit. */
            (void)pthread_mutex_lock(&batch.mutex);
            create_status = batch.stop ? 1 : 0;
            (void)pthread_mutex_unlock(&batch.mutex);
            if (create_status != 0) {
                break;
            }

            create_status = pthread_create(&threads[created], NULL,
                                           batch_worker, &batch);
            if (create_status != 0) {
                create_failed = true;
                batch_request_stop(&batch);
                break;
            }
            created++;
        }

        /* Every created worker is joined on both success and failure. */
        for (i = 0U; i < created; i++) {
            if (pthread_join(threads[i], NULL) != 0) {
                join_failed = true;
            }
        }
        (void)pthread_mutex_destroy(&batch.mutex);
        free(threads);

        if (create_failed) {
            r = COG_ERROR_NOMEM;
            goto cleanup;
        }
        if (join_failed) {
            r = COG_ERROR_IO;
            goto cleanup;
        }
    }

    if (batch.stop_on_error && batch.first_error_index < call_count) {
        r = batch.first_error_status;
        goto cleanup;
    }

    /* All workers are joined.  Assemble the collection on this thread. */
    for (i = 0U; i < call_count; i++) {
        total_str += batch.lens[i] + 1U;
        total_str += batch.error_lens[i] + 1U;
    }

    coll = (cog_toolkit_result_collection_t *)malloc(sizeof(*coll));
    if (coll == NULL) {
        r = COG_ERROR_NOMEM;
        goto cleanup;
    }
    coll->count = call_count;
    coll->results = NULL;
    coll->storage = NULL;
    if (call_count > 0U) {
        coll->results = (cog_toolkit_result_t *)calloc(
            call_count, sizeof(cog_toolkit_result_t));
        if (coll->results == NULL) {
            r = COG_ERROR_NOMEM;
            goto cleanup;
        }
        if (total_str > 0U) {
            coll->storage = (char *)malloc(total_str);
            if (coll->storage == NULL) {
                r = COG_ERROR_NOMEM;
                goto cleanup;
            }
        }
    }

    sp = coll->storage;
    for (i = 0U; i < call_count; i++) {
        cog_toolkit_result_t *res = &coll->results[i];

        res->status = batch.statuses[i];
        res->output_len = batch.lens[i];
        if (batch.lens[i] > 0U) {
            (void)memcpy(sp, batch.bufs[i], batch.lens[i]);
            sp[batch.lens[i]] = '\0';
            res->output = sp;
            sp += batch.lens[i] + 1U;
        } else {
            res->output = NULL;
        }
        if (batch.error_lens[i] > 0U) {
            (void)memcpy(sp, batch.errors[i], batch.error_lens[i]);
            sp[batch.error_lens[i]] = '\0';
            res->error_message = sp;
            sp += batch.error_lens[i] + 1U;
        } else {
            res->error_message = NULL;
        }
    }

    *out_results = coll;
    coll = NULL;

cleanup:
    if (coll != NULL) {
        free(coll->results);
        free(coll->storage);
        free(coll);
    }
    if (batch.bufs != NULL && batch.errors != NULL) {
        for (i = 0U; i < call_count; i++) {
            free(batch.bufs[i]);
            free(batch.errors[i]);
        }
    }
    free(batch.bufs);
    free(batch.caps);
    free(batch.lens);
    free(batch.statuses);
    free(batch.errors);
    free(batch.error_lens);
    return r;
}
