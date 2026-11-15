/** @file toolkit_exec.c @brief At-most-once synchronous compatibility dispatch. */
#define _POSIX_C_SOURCE 200809L
#include <cog/toolkit_exec.h>
#include <cog/core.h>
#include <cog/session_store.h>
#include "toolkit_internal.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t clock_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) { return 0U; }
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static void invoke(cog_core_t *core, const cog_tool_table_t *table,
                   cog_toolkit_call_t *call, uint32_t default_timeout)
{
    if (call->invoked) { return; }
    call->invoked = true;
    cog_toolkit_result_t *out = &call->outcome;
    out->output = call->capture;
    if (call->cancelled) { out->status = COG_ERROR_ABORTED; call->finished = true; return; }
    const cog_tool_t *tool;
    out->status = cog_tool_table_lookup(table, call->name, &tool);
    if (out->status != COG_OK) { call->finished = true; return; }
    if (tool->handler == NULL) { out->status = COG_ERROR_UNSUPPORTED; call->finished = true; return; }
    uint32_t timeout = call->timeout_ms != 0U ? call->timeout_ms : default_timeout;
    uint64_t began = clock_ms();
    size_t length = 0U;
    out->side_effects_possible = true;
    out->status = tool->handler(core, call->request_len != 0U ? call->request : NULL,
        call->request_len, call->capture, COG_TOOLKIT_CAPTURE_BYTES, &length);
    if (out->status == COG_OK) {
        if (length >= COG_TOOLKIT_CAPTURE_BYTES || call->capture[length] != '\0') {
            out->status = COG_ERROR_OVERFLOW;
        } else { out->output_len = length; }
    }
    if (out->status == COG_ERROR_OVERFLOW) {
        out->output_len = length;
        out->truncated = true;
        call->capture[0] = '\0';
    }
    if (out->status == COG_OK && clock_ms() - began >= (uint64_t)timeout) {
        out->status = COG_ERROR_TIMEOUT;
    }
    if (call->cancelled) { out->status = COG_ERROR_ABORTED; }
    call->finished = true;
    if (out->status != COG_OK) { out->error_message = "tool execution failed; outcome retained"; }
}

cog_result_t cog_toolkit_execute(cog_core_t *core, const cog_tool_table_t *table,
    cog_toolkit_call_t *call, cog_toolkit_result_t *result, size_t result_capacity)
{
    if (core == NULL || table == NULL || call == NULL || result == NULL ||
        result->output == NULL) { return COG_ERROR_NULL_PTR; }
    if (result_capacity == 0U) { return COG_ERROR_INVALID_ARG; }
    if (cog_store_requires_owner(cog_core_store(core))) { return COG_ERROR_UNSUPPORTED; }
    invoke(core, table, call, 60000U);
    char *destination = result->output;
    *result = call->outcome;
    result->output = destination;
    if (call->outcome.status == COG_ERROR_OVERFLOW) { return COG_ERROR_OVERFLOW; }
    if (call->outcome.output_len >= result_capacity) {
        result->status = COG_ERROR_OVERFLOW;
        return COG_ERROR_OVERFLOW;
    }
    memcpy(destination, call->capture, call->outcome.output_len + 1U);
    return result->status;
}

typedef struct batch {
    cog_core_t *core;
    const cog_tool_table_t *table;
    cog_toolkit_call_t *const *calls;
    size_t count;
    size_t next;
    uint32_t timeout;
    bool stop_on_error;
    bool stop;
    bool ready;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    cog_toolkit_result_collection_t *collection;
} batch_t;

static void run_index(batch_t *batch, size_t index)
{
    cog_toolkit_call_t *call = batch->calls[index];
    invoke(batch->core, batch->table, call, batch->timeout);
    cog_toolkit_result_t *result = &batch->collection->results[index];
    char *output = result->output;
    *result = call->outcome;
    result->output = output;
    if (result->status != COG_ERROR_OVERFLOW) {
        memcpy(output, call->capture, call->outcome.output_len + 1U);
        if (call->outcome.output_len == 0U) { result->output = NULL; }
    }
}

static void *worker(void *argument)
{
    batch_t *batch = argument;
    (void)pthread_mutex_lock(&batch->mutex);
    while (!batch->ready) { (void)pthread_cond_wait(&batch->condition, &batch->mutex); }
    for (;;) {
        if (batch->stop || batch->next == batch->count) { break; }
        size_t index = batch->next++;
        (void)pthread_mutex_unlock(&batch->mutex);
        run_index(batch, index);
        (void)pthread_mutex_lock(&batch->mutex);
        if (batch->stop_on_error && batch->collection->results[index].status != COG_OK) {
            batch->stop = true;
        }
    }
    (void)pthread_mutex_unlock(&batch->mutex);
    return NULL;
}

cog_result_t cog_toolkit_execute_all(cog_core_t *core, const cog_tool_table_t *table,
    cog_toolkit_call_t *const *calls, size_t call_count,
    const cog_toolkit_exec_opts_t *opts, cog_toolkit_result_collection_t **out_results)
{
    if (out_results == NULL) { return COG_ERROR_NULL_PTR; }
    *out_results = NULL;
    if (core == NULL || table == NULL || calls == NULL) { return COG_ERROR_NULL_PTR; }
    if (cog_store_requires_owner(cog_core_store(core))) { return COG_ERROR_UNSUPPORTED; }
    size_t parallel = opts != NULL && opts->max_parallel != 0U ? opts->max_parallel : 1U;
    for (size_t i = 0U; i < call_count; i++) {
        if (calls[i] == NULL) { return COG_ERROR_INVALID_ARG; }
        if (parallel > 1U) {
            const cog_tool_t *tool;
            cog_result_t r = cog_tool_table_lookup(table, calls[i]->name, &tool);
            if (r != COG_OK) { return r; }
            if ((tool->flags & (COG_TOOL_REENTRANT | COG_TOOL_INDEPENDENT)) !=
                (COG_TOOL_REENTRANT | COG_TOOL_INDEPENDENT)) { return COG_ERROR_INVALID_ARG; }
            for (size_t j = 0U; j < i; j++) {
                if (calls[i] == calls[j]) { return COG_ERROR_INVALID_ARG; }
            }
        }
    }
    if (call_count > SIZE_MAX / COG_TOOLKIT_CAPTURE_BYTES ||
        call_count > SIZE_MAX / sizeof(cog_toolkit_result_t)) { return COG_ERROR_LIMIT; }
    cog_toolkit_result_collection_t *collection = calloc(1U, sizeof(*collection));
    if (collection == NULL) { return COG_ERROR_NOMEM; }
    collection->count = call_count;
    if (call_count != 0U) {
        collection->results = calloc(call_count, sizeof(*collection->results));
        collection->storage = calloc(call_count, COG_TOOLKIT_CAPTURE_BYTES);
        if (collection->results == NULL || collection->storage == NULL) {
            cog_toolkit_result_collection_free(collection); return COG_ERROR_NOMEM;
        }
        for (size_t i = 0U; i < call_count; i++) {
            collection->results[i].status = COG_ERROR_ABORTED;
            collection->results[i].output = collection->storage + i * COG_TOOLKIT_CAPTURE_BYTES;
        }
    }
    batch_t batch = {0};
    batch.core = core;
    batch.table = table;
    batch.calls = calls;
    batch.count = call_count;
    batch.timeout = opts != NULL && opts->timeout_ms != 0U ? opts->timeout_ms : 60000U;
    batch.stop_on_error = opts != NULL && opts->stop_on_error;
    batch.collection = collection;
    if (parallel > call_count) { parallel = call_count; }
    if (parallel <= 1U) {
        for (size_t i = 0U; i < call_count; i++) {
            run_index(&batch, i);
            if (batch.stop_on_error && collection->results[i].status != COG_OK) { break; }
        }
    } else {
        pthread_t *threads = calloc(parallel, sizeof(*threads));
        if (threads == NULL) { cog_toolkit_result_collection_free(collection); return COG_ERROR_NOMEM; }
        if (pthread_mutex_init(&batch.mutex, NULL) != 0) {
            free(threads); cog_toolkit_result_collection_free(collection); return COG_ERROR_NOMEM;
        }
        if (pthread_cond_init(&batch.condition, NULL) != 0) {
            (void)pthread_mutex_destroy(&batch.mutex);
            free(threads); cog_toolkit_result_collection_free(collection); return COG_ERROR_NOMEM;
        }
        size_t created = 0U;
        for (; created < parallel; created++) {
            if (pthread_create(&threads[created], NULL, worker, &batch) != 0) { break; }
        }
        (void)pthread_mutex_lock(&batch.mutex);
        batch.stop = created != parallel;
        batch.ready = true;
        (void)pthread_cond_broadcast(&batch.condition);
        (void)pthread_mutex_unlock(&batch.mutex);
        bool join_failed = false;
        for (size_t i = 0U; i < created; i++) {
            if (pthread_join(threads[i], NULL) != 0) { join_failed = true; }
        }
        (void)pthread_cond_destroy(&batch.condition);
        (void)pthread_mutex_destroy(&batch.mutex);
        free(threads);
        if (created != parallel) { cog_toolkit_result_collection_free(collection); return COG_ERROR_NOMEM; }
        if (join_failed) { /* Valid joinable IDs on the owning thread cannot fail. */
            *out_results = collection;
            return COG_OK;
        }
    }
    *out_results = collection;
    return COG_OK;
}
