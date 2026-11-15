/**
 * @file toolkit_exec.h
 * @brief Tool execution dispatcher.
 *
 * The execution dispatcher is the entry point for running tools from the
 * static tool table defined in cog-core.  It resolves the tool by name,
 * prepares the execution context, invokes the tool implementation, and
 * writes the outcome into a caller-provided result value or an opaque
 * result collection.
 *
 * Threading model: @c cog_toolkit_execute is single-threaded and runs on
 * the caller's thread.  @c cog_toolkit_execute_all is single-threaded
 * when @c max_parallel is 1 (the fast path), and may spawn up to
 * @c max_parallel worker threads otherwise.  The caller is blocked until
 * every launched worker has joined.  Concurrent calls on the same core or
 * tool table still require external synchronisation.  When
 * @c max_parallel is greater than 1, individual tool handlers may run
 * concurrently and must therefore be reentrant; they must not mutate
 * @p core or other shared state without their own synchronisation.
 */
#ifndef COG_TOOLKIT_EXEC_H
#define COG_TOOLKIT_EXEC_H

#include <cog/result.h>
#include <cog/tool.h>
#include <cog/toolkit_call.h>
#include <cog/toolkit_result.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration to break the core/toolkit include cycle. */
typedef struct cog_core cog_core_t;

/**
 * @brief Options controlling parallel tool execution.
 *
 * Callers should zero-initialise this struct to obtain defaults; adding
 * fields later will not break callers that zero-init.
 */
typedef struct cog_toolkit_exec_opts {
    /**
     * @brief Per-call wall-clock timeout in milliseconds.
     *
     * Zero disables the explicit timeout and lets the dispatcher apply a
     * sensible default.
     */
    uint32_t timeout_ms;

    /**
     * @brief Maximum number of tool calls to run concurrently.
     *
     * One means sequential execution on the caller's thread (fast path).
     * Zero selects a sensible default, typically the number of calls.
     * Values greater than one may spawn up to that many worker threads;
     * the caller is blocked until all workers join.
     */
    size_t max_parallel;

    /**
     * @brief If true, stop launching new calls after the first observed
     * failure.
     *
     * Already-running calls are still awaited, but if any call fails the
     * final return is that failure code and @p *out_results is set to
     * NULL.  Because tools are synchronous and non-preemptive, a failure
     * observed in one worker cannot abort calls already running in other
     * workers.
     */
    bool stop_on_error;
} cog_toolkit_exec_opts_t;

/**
 * @brief Execute a single tool call.
 *
 * Looks up @p call->name in @p table, invokes the matching tool function
 * with the request payload recorded in @p call, and writes the outcome
 * into @p result.  The caller must provide a writable output buffer via
 * @p result->output with a capacity of at least @p result_capacity bytes.
 *
 * @param[in,out] core            Core instance that requested the tool.
 * @param[in]     table           Static const tool table.
 * @param[in,out] call            Tool call context; carries name, request,
 *                                timeout, and cancellation state.
 * @param[in,out] result          Receives the execution result; the caller
 *                                must set @p result->output to a writable
 *                                buffer of size @p result_capacity.
 * @param[in]     result_capacity Size of the output buffer in bytes,
 *                                including space for a NUL terminator.
 *                                Must be greater than zero.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p core, @p table, @p call, or
 *         @p result is NULL, or if @p result->output is NULL.
 * @return COG_ERROR_INVALID_ARG if @p result_capacity is zero.
 * @return COG_ERROR_NOT_FOUND if @p call->name does not appear in
 *         @p table.
 * @return COG_ERROR_ABORTED if the call was cancelled before or during
 *         execution.
 * @return Another error code propagated from the tool implementation.
 *
 * @pre @p core is a fully initialised core instance.
 * @pre @p table is valid for the duration of the call.
 * @pre @p result->output points to a writable buffer of at least
 *      @p result_capacity bytes.
 *
 * @post On success, @p result->status is @c COG_OK and @p result->output
 *       contains a NUL-terminated result string.
 * @post On failure, @p result->status is non-zero and @p result->output
 *       is unchanged.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_toolkit_execute(cog_core_t *core,
                                 const cog_tool_table_t *table,
                                 cog_toolkit_call_t *call,
                                 cog_toolkit_result_t *result,
                                 size_t result_capacity);

/**
 * @brief Execute multiple tool calls.
 *
 * Dispatches each call in @p calls according to @p opts.  Results are
 * collected into an opaque @c cog_toolkit_result_collection_t owned by
 * the caller.  The collection preserves the same order as @p calls.
 *
 * @param[in,out] core        Core instance that requested the tools.
 * @param[in]     table       Static const tool table.
 * @param[in,out] calls       Array of call contexts; each must be non-NULL.
 * @param[in]     call_count  Number of entries in @p calls; may be zero.
 * @param[in]     opts        Execution options; may be NULL for defaults.
 * @param[out]    out_results Receives the result collection on success;
 *                            set to NULL on failure.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p core, @p table, @p calls, or
 *         @p out_results is NULL.
 * @return COG_ERROR_INVALID_ARG if @p call_count is non-zero and any
 *         entry in @p calls is NULL.
 * @return COG_ERROR_NOMEM if allocation or worker-thread creation fails.
 * @return The first failing result code by call index when
 *         @p opts->stop_on_error is true and any call fails.
 *
 * @pre @p core is a fully initialised core instance.
 * @pre @p table is valid for the duration of the call.
 * @pre If @p call_count is non-zero, every entry in @p calls is valid.
 *
 * @post On success, @p *out_results is a valid collection containing
 *       exactly @p call_count results in the same order as @p calls.
 * @post On failure, @p *out_results is NULL and no resources are held.
 *
 * Threading: this call blocks the caller until all dispatched calls
 * complete.  For @p opts->max_parallel greater than 1 worker threads are
 * spawned; the tool table is read-only and shared, but tool handlers
 * must be reentrant.  Do not modify @p calls or the call contexts from
 * another thread during the call unless externally synchronised.
 */
cog_result_t cog_toolkit_execute_all(cog_core_t *core,
                                     const cog_tool_table_t *table,
                                     cog_toolkit_call_t *const *calls,
                                     size_t call_count,
                                     const cog_toolkit_exec_opts_t *opts,
                                     cog_toolkit_result_collection_t **out_results);

#ifdef __cplusplus
}
#endif

#endif /* COG_TOOLKIT_EXEC_H */
