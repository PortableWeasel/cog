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
     * Zero selects one (sequential); parallelism is always explicit.
     * Values greater than one may spawn up to that many worker threads;
     * the caller is blocked until all workers join.
     */
    size_t max_parallel;

    /**
     * @brief If true, stop launching new calls after the first observed
     * failure.
     *
     * Already-running calls are still awaited. Their results and all failures
     * are retained in the returned collection; unstarted calls receive ABORTED.  Because tools are synchronous and non-preemptive, a failure
     * observed in one worker cannot abort calls already running in other
     * workers.
     */
    bool stop_on_error;
} cog_toolkit_exec_opts_t;

/**
 * @brief Execute a single tool call.
 *
 * At-most-once: a call context invokes its handler no more than once. The
 * handler receives its constructor-reserved 64 KiB capture buffer. A later
 * execute copies the same cached outcome; caller capacity OVERFLOW reports
 * output_len and leaves output bytes unchanged. It NEVER retries the handler.
 * Handler capture overflow is a terminal OVERFLOW with side_effects_possible;
 * no automatic rerun/growth is allowed. Legacy synchronous handlers may block.
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
 * @post On delivery OVERFLOW, result->output_len is required excluding NUL,
 *       result->status is OVERFLOW, and result->output bytes are unchanged.
 *       The invocation outcome remains cached for safe larger-buffer delivery.
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
 * Each call is still invoked at most once; no output overflow retry may
 * reinvoke a handler. Terminal outcomes of every launched call are retained
 * even if stop_on_error is true; return COG_OK with collection, inspect each
 * result. Only pre-dispatch/setup failure returns no collection. Concurrent
 * dispatch requires every tool REENTRANT|INDEPENDENT; otherwise INVALID_ARG
 * before any tool runs. This blocking compatibility API is not used by the
 * responsive real-client loop. The call is also an explicit result-collection
 * constructor: reserve every output/status/worker slot before starting any
 * handler; worker setup uses a startup barrier so partial thread creation
 * failure cannot execute tools without a retained outcome collection.
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
 * @return COG_OK with retained outcomes after dispatch, including failures;
 *         callers inspect each result, never infer no side effects from error.
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

/** @brief Opaque asynchronous execution; owns copied inputs and bounded output. */
typedef struct cog_toolkit_execution cog_toolkit_execution_t;
/** @brief Explicit policy decision; zero initialization never permits execution. */
typedef enum cog_toolkit_policy_decision {
    COG_TOOLKIT_POLICY_UNDECIDED = 0,
    COG_TOOLKIT_POLICY_DENY,
    COG_TOOLKIT_POLICY_ALLOW
} cog_toolkit_policy_decision_t;
/**
 * @brief Construct/validate one complete identified tool call without execution.
 * @param[out] out NULL on failure; caller-owned execution handle on success.
 * @param[in,out] core Borrowed core, valid through execution_free.
 * @param[in] table Static const descriptors, valid through execution_free.
 * @param[in] call_id Nonempty stable model call ID, copied.
 * @param[in] name Complete tool name, copied and resolved.
 * @param[in] arguments Complete strict JSON object, copied and validated against
 * descriptor input_schema. Streaming fragments must never reach this API.
 * @param[in] arguments_len Exact bytes excluding NUL.
 * @param[in] config Explicit nonzero runtime/output bounds and borrowed user.
 * @return COG_OK; NULL_PTR; INVALID_ARG for malformed arguments/schema; NOT_FOUND;
 * UNSUPPORTED for no async vtable or schema keyword; LIMIT/NOMEM for capacity.
 * INVALID_ARG if any required async callback is NULL. Validate all six before
 * invoking new_ or any implementation callback; never publish a partial handle.
 * @note If the core store requires ownership, config.owner_guard is mandatory
 * and must match this session. Constructor borrows it; start validates it before
 * effects; free returns it only after terminal cleanup. Missing/stale guard
 * returns INVALID_STATE. Legacy synchronous execute/execute_all refuse such
 * contexts with UNSUPPORTED; they cannot bypass the fenced async profile.
 * Owner thread. Constructor reserves all runtime Cog storage and invokes
 * only tool.new_, whose contract forbids external effects. Schema subset:
 * type/object/array/string/number/integer/boolean/null, properties, required,
 * additionalProperties=false, items, enum, minimum/maximum, min/maxLength and
 * min/maxItems. Annotation keywords title/description/$schema are accepted;
 * unknown validation keywords fail UNSUPPORTED, never ignored. Codec/parser
 * pools are part of this constructor, not process-global state. Argument,
 * schema, depth and pool ceilings are explicit in config; exceeding any returns
 * LIMIT before effects. Reject duplicate keys at every level and invalid UTF-8.
 * Length keywords count Unicode scalar values, array limits count entries;
 * integer means an exact mathematical integer in signed 64-bit range; number
 * means a finite representable double. Unsupported numeric range/schema forms
 * return UNSUPPORTED, malformed values INVALID_ARG. No remote schema fetch,
 * regex, $ref, coercion, default insertion or unknown-keyword acceptance.
 */
cog_result_t cog_toolkit_execution_new(cog_toolkit_execution_t **out,
                                       cog_core_t *core,
                                       const cog_tool_table_t *table,
                                       const char *call_id, const char *name,
                                       const char *arguments, size_t arguments_len,
                                       const cog_tool_async_config_t *config);
/**
 * @brief Admit one policy decision and start at most once.
 * @param[in,out] execution Prepared handle.
 * @param[in] decision ALLOW executes; DENY creates terminal DENIED result;
 * UNDECIDED returns AGAIN without effects.
 * @param[in] now_ms Monotonic clock shared with poll/cancel.
 * @return COG_OK; AGAIN; NULL_PTR; INVALID_ARG; INVALID_STATE if already started
 * or the current session has become INDETERMINATE/unsafe. Revalidate at ALLOW
 * start, not only constructor/guard acquisition. No new effect after uncertainty.
 * @pre Before ALLOW, application durably saved the corresponding STARTED ledger
 * and applied its workspace/command policy. An interactive prompt occurs in the
 * app while this handle remains prepared, never inside a blocking callback.
 * @note Owner thread. Decision fixed after admission. No hidden permission default.
 */
cog_result_t cog_toolkit_execution_start(cog_toolkit_execution_t *execution,
                                         cog_toolkit_policy_decision_t decision,
                                         uint64_t now_ms);
/**
 * @brief Advance tool/process cleanup without sleeping; no output delivery.
 * @param[in,out] execution Handle.
 * @param[in] now_ms Nondecreasing monotonic time.
 * @param[out] out_done True only after output finalized and owned children reaped.
 * @param[out] out_next_deadline_ms Absolute deadline or UINT64_MAX.
 * @return COG_OK; NULL_PTR; INVALID_ARG; INVALID_STATE before admission; driver
 * failure propagated. Execution failures are retained result.status values.
 * @note Owner thread; call from fair core service even under sustained traffic.
 */
cog_result_t cog_toolkit_execution_poll(cog_toolkit_execution_t *execution,
                                        uint64_t now_ms, bool *out_done,
                                        uint64_t *out_next_deadline_ms);
/**
 * @brief Request cancellation, then poll until process group is reaped.
 * @param[in,out] execution Handle; prepared cancellation yields terminal ABORTED.
 * @param[in] now_ms Monotonic time.
 * @return COG_OK (idempotent); NULL_PTR; INVALID_ARG for backwards time.
 * @note Owner thread, never signal-safe, never blocks waiting for a child.
 */
cog_result_t cog_toolkit_execution_cancel(cog_toolkit_execution_t *execution,
                                          uint64_t now_ms);
/**
 * @brief Copy retained final output without executing the tool again.
 * @param[in] execution Handle, terminal required.
 * @param[out] out_result Metadata; unchanged on failure except output_len on OVERFLOW.
 * @param[out] output Caller bytes; NULL iff capacity=0.
 * @param[in] capacity Bytes including NUL.
 * @param[out] out_required Exact output bytes including NUL.
 * @return COG_OK even for denied/failed tool (inspect status); AGAIN until done;
 * OVERFLOW writes required/output_len only, preserves bytes and retained result;
 * NULL_PTR/INVALID_ARG. Any number of copies is safe and has no external effects.
 * @note Owner thread. output is caller-owned; ID/error pointers remain borrowed
 * until execution_free. Truncation is explicit and never triggers tool replay.
 */
cog_result_t cog_toolkit_execution_result(const cog_toolkit_execution_t *execution,
                                          cog_toolkit_result_t *out_result,
                                          char *output, size_t capacity,
                                          size_t *out_required);
/**
 * @brief Release a prepared or terminal execution and all constructor storage.
 * @param[in,out] execution NULL is COG_OK.
 * @return COG_OK; INVALID_STATE if active/cancelling (cancel/poll first).
 * @note Owner thread. On failure handle is intact. Never abandons live children.
 */
cog_result_t cog_toolkit_execution_free(cog_toolkit_execution_t *execution);

#ifdef __cplusplus
}
#endif

#endif /* COG_TOOLKIT_EXEC_H */
