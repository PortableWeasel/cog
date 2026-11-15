/**
 * @file toolkit_exec.c
 * @brief Implementation of the tool execution dispatcher declared in
 *        cog/toolkit_exec.h.
 *
 * The dispatcher resolves a tool by name in the static const tool table,
 * invokes its @c cog_tool_fn implementation, and writes the outcome into
 * a caller-provided result value or an opaque result collection.
 *
 * The dispatcher is single-threaded by design: calls are run to
 * completion on the invoking thread.  "Parallel" execution is cooperative
 * and bounded by @c max_parallel; because tools are synchronous C
 * functions that cannot be preempted, @c cog_toolkit_execute_all runs the
 * calls sequentially while preserving call order in the result collection.
 *
 * Timeout handling is advisory and enforced at the boundaries of each
 * tool invocation: a per-call wall-clock deadline is computed from the
 * call's timeout (or the dispatcher default) and checked after the tool
 * returns.  A tool that overruns its budget is recorded as
 * @c COG_ERROR_TIMEOUT.  Cancellation is cooperative: the flag is checked
 * before and after each invocation, and a cancelled call is recorded as
 * @c COG_ERROR_ABORTED.
 *
 * The module holds no global mutable state.
 *
 * Ticket: 8d52c1a (Stage 12.4b — Implement toolkit)
 */

#define _POSIX_C_SOURCE 200809L

#include "cog/toolkit_exec.h"
#include "toolkit_internal.h"

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
 * @brief Invoke a tool, growing the output buffer on overflow.
 *
 * @p out_buf / @p out_cap describe a caller-owned buffer.  If @p out_buf
 * is NULL an initial buffer is allocated.  On @c COG_ERROR_OVERFLOW the
 * buffer is grown to the required size and the tool is re-invoked.  On
 * success @p out_len receives the result length (excluding the NUL).
 *
 * The caller owns the returned buffer and must free it.
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
                /* realloc leaves buf intact on failure; caller frees it. */
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

/**
 * @brief Build a human-readable error message for a failed call.
 *
 * Returns a heap-allocated NUL-terminated string, or NULL on allocation
 * failure (in which case @p out_len is set to zero).
 */
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
    uint32_t default_timeout;
    cog_result_t r = COG_OK;
    size_t i;
    cog_toolkit_result_collection_t *coll = NULL;
    char **tmp_bufs = NULL;
    size_t *tmp_caps = NULL;
    size_t *tmp_lens = NULL;
    cog_result_t *tmp_status = NULL;
    char **tmp_err = NULL;
    size_t *tmp_err_lens = NULL;
    size_t total_str = 0U;
    char *sp;

    if (core == NULL || table == NULL || calls == NULL || out_results == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    *out_results = NULL;

    if (call_count != 0U) {
        for (i = 0U; i < call_count; i++) {
            if (calls[i] == NULL) {
                return COG_ERROR_INVALID_ARG;
            }
        }
    }

    /* Resolve options.  Zero-initialised fields select defaults. */
    if (opts != NULL) {
        local_opts = *opts;
    } else {
        local_opts.timeout_ms = 0U;
        local_opts.max_parallel = 0U;
        local_opts.stop_on_error = false;
    }
    default_timeout = local_opts.timeout_ms != 0U
                          ? local_opts.timeout_ms
                          : COG_TOOLKIT_DEFAULT_TIMEOUT_MS;

    if (call_count > 0U) {
        tmp_bufs = (char **)calloc(call_count, sizeof(char *));
        tmp_caps = (size_t *)calloc(call_count, sizeof(size_t));
        tmp_lens = (size_t *)calloc(call_count, sizeof(size_t));
        tmp_status = (cog_result_t *)calloc(call_count, sizeof(cog_result_t));
        tmp_err = (char **)calloc(call_count, sizeof(char *));
        tmp_err_lens = (size_t *)calloc(call_count, sizeof(size_t));
        if (tmp_bufs == NULL || tmp_caps == NULL || tmp_lens == NULL ||
            tmp_status == NULL || tmp_err == NULL || tmp_err_lens == NULL) {
            r = COG_ERROR_NOMEM;
            goto fail;
        }
    }

    /* Run each call, recording status, output length, and error message. */
    for (i = 0U; i < call_count; i++) {
        cog_toolkit_call_t *call = calls[i];
        const cog_tool_t *tool = NULL;
        uint32_t timeout_ms;
        uint64_t deadline;
        size_t len = 0U;
        const char *request;
        size_t request_len;

        tmp_status[i] = COG_OK;

        if (cog_toolkit_call_is_cancelled(call)) {
            tmp_status[i] = COG_ERROR_ABORTED;
            tmp_err[i] = make_error_message(call, COG_ERROR_ABORTED,
                                            &tmp_err_lens[i]);
            goto after_call;
        }

        r = cog_tool_table_lookup(table, cog_toolkit_call_name(call), &tool);
        if (r != COG_OK) {
            tmp_status[i] = r;
            tmp_err[i] = make_error_message(call, r, &tmp_err_lens[i]);
            goto after_call;
        }

        timeout_ms = cog_toolkit_call_timeout_ms(call);
        if (timeout_ms == 0U) {
            timeout_ms = default_timeout;
        }
        deadline = now_ms() + (uint64_t)timeout_ms;

        request = cog_toolkit_call_request(call, &request_len);
        r = invoke_tool(core, tool, request, request_len,
                        &tmp_bufs[i], &tmp_caps[i], &len);
        if (r != COG_OK) {
            tmp_status[i] = r;
            tmp_err[i] = make_error_message(call, r, &tmp_err_lens[i]);
            goto after_call;
        }
        if (now_ms() >= deadline) {
            tmp_status[i] = COG_ERROR_TIMEOUT;
            tmp_err[i] = make_error_message(call, COG_ERROR_TIMEOUT,
                                            &tmp_err_lens[i]);
            goto after_call;
        }
        if (cog_toolkit_call_is_cancelled(call)) {
            tmp_status[i] = COG_ERROR_ABORTED;
            tmp_err[i] = make_error_message(call, COG_ERROR_ABORTED,
                                            &tmp_err_lens[i]);
            goto after_call;
        }
        tmp_lens[i] = len;

    after_call:
        if (tmp_status[i] != COG_OK && local_opts.stop_on_error) {
            r = tmp_status[i];
            goto fail;
        }
    }

    /* Compute the total string storage needed by the collection. */
    for (i = 0U; i < call_count; i++) {
        total_str += tmp_lens[i] + 1U;
        total_str += tmp_err_lens[i] + 1U;
    }

    coll = (cog_toolkit_result_collection_t *)malloc(sizeof(*coll));
    if (coll == NULL) {
        r = COG_ERROR_NOMEM;
        goto fail;
    }
    coll->count = call_count;
    coll->results = NULL;
    coll->storage = NULL;
    if (call_count > 0U) {
        coll->results = (cog_toolkit_result_t *)calloc(
            call_count, sizeof(cog_toolkit_result_t));
        if (coll->results == NULL) {
            r = COG_ERROR_NOMEM;
            goto fail;
        }
        if (total_str > 0U) {
            coll->storage = (char *)malloc(total_str);
            if (coll->storage == NULL) {
                r = COG_ERROR_NOMEM;
                goto fail;
            }
        }
    }

    /* Copy each result's output and error message into the collection's
     * single storage block. */
    sp = coll->storage;
    for (i = 0U; i < call_count; i++) {
        cog_toolkit_result_t *res = &coll->results[i];

        res->status = tmp_status[i];
        res->output_len = tmp_lens[i];
        if (tmp_lens[i] > 0U) {
            (void)memcpy(sp, tmp_bufs[i], tmp_lens[i]);
            sp[tmp_lens[i]] = '\0';
            res->output = sp;
            sp += tmp_lens[i] + 1U;
        } else {
            res->output = NULL;
        }
        if (tmp_err_lens[i] > 0U) {
            (void)memcpy(sp, tmp_err[i], tmp_err_lens[i]);
            sp[tmp_err_lens[i]] = '\0';
            res->error_message = sp;
            sp += tmp_err_lens[i] + 1U;
        } else {
            res->error_message = NULL;
        }
    }

    /* Release temporary per-call buffers. */
    for (i = 0U; i < call_count; i++) {
        free(tmp_bufs[i]);
        free(tmp_err[i]);
    }
    free(tmp_bufs);
    free(tmp_caps);
    free(tmp_lens);
    free(tmp_status);
    free(tmp_err);
    free(tmp_err_lens);

    *out_results = coll;
    return COG_OK;

fail:
    if (coll != NULL) {
        free(coll->results);
        free(coll->storage);
        free(coll);
    }
    /* Only dereference the per-call arrays when both were allocated.  On
     * a partial allocation failure the individual entries are all NULL
     * (calloc), so freeing the arrays themselves is sufficient. */
    if (tmp_bufs != NULL && tmp_err != NULL) {
        for (i = 0U; i < call_count; i++) {
            free(tmp_bufs[i]);
            free(tmp_err[i]);
        }
    }
    free(tmp_bufs);
    free(tmp_caps);
    free(tmp_lens);
    free(tmp_status);
    free(tmp_err);
    free(tmp_err_lens);
    *out_results = NULL;
    return r;
}
