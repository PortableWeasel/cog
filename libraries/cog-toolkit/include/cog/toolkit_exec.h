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
 * Threading model: the dispatcher is single-threaded by design.  A single
 * @c cog_toolkit_execute or @c cog_toolkit_execute_all call must run to
 * completion on the thread that invoked it.  Concurrent calls on the same
 * core or tool table require external synchronisation.
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
     * One means sequential execution.  Zero selects a sensible default
     * (typically the number of calls).
     */
    size_t max_parallel;

    /**
     * @brief If true, stop launching new calls as soon as one fails.
     *
     * Already-running calls are still awaited and their results are
     * recorded.
     */
    bool stop_on_error;
} cog_toolkit_exec_opts_t;

/**
 * @brief Execute a single tool call.
 *
 * Looks up @p call->name in @p table, invokes the matching tool function
 * with the request payload recorded in @p call, and writes the outcome
 * into @p result.  The caller must provide a writable output buffer in
 * @p result->output with @p result->output_capacity greater than zero.
 *
 * @param[in,out] core   Core instance that requested the tool.
 * @param[in]     table  Static const tool table.
 * @param[in,out] call   Tool call context; carries name, request,
 *                       timeout, and cancellation state.
 * @param[out]    result Receives the execution result.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p core, @p table, @p call, or
 *         @p result is NULL, or if @p result->output is NULL.
 * @return COG_ERROR_INVALID_ARG if @p result->output_capacity is zero.
 * @return COG_ERROR_NOT_FOUND if @p call->name does not appear in
 *         @p table.
 * @return COG_ERROR_ABORTED if the call was cancelled before or during
 *         execution.
 * @return Another error code propagated from the tool implementation.
 *
 * @pre @p core is a fully initialised core instance.
 * @pre @p table is valid for the duration of the call.
 * @pre @p result->output points to a writable buffer of at least
 *      @p result->output_capacity bytes.
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
                                 cog_toolkit_result_t *result);

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
 *                            unchanged on failure.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p core, @p table, @p calls, or
 *         @p out_results is NULL.
 * @return COG_ERROR_INVALID_ARG if @p call_count is non-zero and any
 *         entry in @p calls is NULL.
 * @return COG_ERROR_NOMEM if allocation fails.
 * @return Another error code propagated from the first failing tool
 *         when @p opts->stop_on_error is true.
 *
 * @pre @p core is a fully initialised core instance.
 * @pre @p table is valid for the duration of the call.
 * @pre If @p call_count is non-zero, every entry in @p calls is valid.
 *
 * @post On success, @p *out_results is a valid collection containing
 *       exactly @p call_count results in the same order as @p calls.
 * @post On failure, @p *out_results is NULL and no resources are held.
 *
 * Threading: not thread-safe.
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
