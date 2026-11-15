/**
 * @file tool.h
 * @brief Tool descriptor table and tool function contract.
 *
 * Tools are plain C functions exposed through a static const table.
 * The core uses the table to resolve tool names invoked by handlers; no
 * dynamic registration is supported.
 *
 * Threading model: the tool table is read-only after construction and may
 * be shared by multiple core instances.  Individual tool implementations
 * must be re-entrant and must not introduce their own global mutable
 * state.
 */
#ifndef COG_TOOL_H
#define COG_TOOL_H

#include <cog/event.h>
#include <cog/result.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration to break the core/tool include cycle. */
typedef struct cog_core cog_core_t;
typedef struct cog_store_owner_guard cog_store_owner_guard_t;

/**
 * @brief Tool implementation signature.
 *
 * The caller supplies the result buffer, so no hidden allocation occurs
 * and ownership never transfers.  If the buffer is too small the tool
 * reports the required length via @p out_result_len (when non-NULL) and
 * returns @c COG_ERROR_OVERFLOW. This is NOT permission to invoke again:
 * side effects may already have occurred. The dispatcher invokes a call at
 * most once, including overflow/error paths, and caches its outcome. Blocking
 * handlers are legacy only and cannot satisfy responsive real-client service.
 * Use the asynchronous table below for process/workspace release tools.
 *
 * @param[in,out] core             Core instance that requested the tool.
 * @param[in]     request          Tool request payload (e.g. JSON arguments).
 * @param[in]     request_len      Length of @p request in bytes.
 * @param[out]    result           Caller-provided buffer that receives the
 *                                 result string.  May be NULL only when
 *                                 @p result_capacity is zero.
 * @param[in]     result_capacity  Size of @p result in bytes, including
 *                                 space for a NUL terminator.
 * @param[out]    out_result_len   On success or @c COG_ERROR_OVERFLOW, set
 *                                 to the length of the result string in
 *                                 bytes (excluding the terminator).  May be
 *                                 NULL if the caller does not need the
 *                                 length.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if a required pointer argument is NULL.
 * @return COG_ERROR_INVALID_ARG if the request is malformed.
 * @return COG_ERROR_NOT_FOUND if the tool is not applicable to the
 *         request.
 * @return COG_ERROR_OVERFLOW if the result does not fit in
 *         @p result_capacity.
 * @return Another error code on failure.
 *
 * @pre @p core is a fully initialised core instance.
 * @pre @p request is valid for the duration of the call.
 * @pre If @p result is non-NULL, @p result_capacity is greater than zero.
 *
 * @post On success, @p result contains a NUL-terminated result string
 *       and ownership remains with the caller.
 * @post On failure, @p result is unchanged.
 */
typedef cog_result_t (*cog_tool_fn)(cog_core_t *core,
                                     const char *request,
                                     size_t request_len,
                                     char *result,
                                     size_t result_capacity,
                                     size_t *out_result_len);

/** @brief Constructor reservation and explicit application-owned tool context. */
typedef struct cog_tool_async_config {
    size_t max_output_bytes; /**< Nonzero capture bound excluding NUL. */
    uint32_t timeout_ms; /**< Nonzero monotonic execution deadline. */
    uint32_t terminate_grace_ms; /**< TERM-to-KILL grace, zero means immediate KILL. */
    void *user; /**< Borrowed workspace/policy config; valid until destroy. */
    cog_store_owner_guard_t *owner_guard; /**< Required for owned-session effects;
       borrowed by execution constructor until destroy, validated before start. */
} cog_tool_async_config_t;

/** @brief Borrowed final tool outcome, retained until destroy. */
typedef struct cog_tool_output {
    cog_result_t status;
    const char *output; /**< UTF-8/text result, NUL terminated; never grows. */
    size_t output_len;
    bool truncated; /**< Capture ceiling reached; explicit, never hidden. */
    bool side_effects_possible; /**< True once an external effect may have begun. */
    int exit_status; /**< Process status only when exit_status_known. */
    bool exit_status_known;
} cog_tool_output_t;

/**
 * @brief Static nonblocking tool operations; no workers are created implicitly.
 *
 * new_ copies request, reserves complete bounded output/state and performs no
 * external side effects. start is called at most once after strict argument
 * schema validation, explicit ALLOW policy, and durable STARTED intent. poll
 * never waits; output is exposed only after done=true, and result is repeatable
 * without execution. cancel is idempotent; it sets no global/signal flag.
 * Process tools own a distinct process group: TERM then deadline-driven KILL,
 * drain pipes and reap all owned children with nonblocking checks before done.
 * A timeout/cancel/error can leave file effects; report side_effects_possible.
 * destroy requires never-started or done, otherwise INVALID_STATE, and returns
 * all constructor resources. No handles/callbacks/child zombies after success.
 *
 * Every callback is owner-thread only, nonrecursive and returns NULL_PTR for
 * required NULLs. new_ returns NOMEM/LIMIT/INVALID_ARG/UNSUPPORTED before effects
 * and sets *out_handle=NULL on failure. start returns INVALID_STATE on repeat;
 * if an effect starts, it returns OK and poll/result expose any ensuing error.
 * poll returns OK with done/deadline (UINT64_MAX if none), INVALID_ARG on a
 * backwards clock, or driver error requiring cancellation. result returns
 * AGAIN until terminal then OK even when output.status is failure. cancel and
 * destroy return OK or INVALID_STATE. No runtime Cog allocations after new_.
 * Descriptors/callers must honor these contracts; the core cannot preempt an
 * arbitrary blocking C callback or provide an OS sandbox.
 */
/**
 * @brief Reserve async tool state without external effects; see table contract.
 * @param[in,out] core Borrowed core.
 * @param[in] request Complete validated arguments, copied during this call.
 * @param[in] request_len Exact argument bytes.
 * @param[in] config Explicit limits and borrowed user context.
 * @param[out] out_handle Owned state on OK, NULL on failure.
 * @return OK, NULL_PTR, INVALID_ARG, LIMIT, NOMEM or UNSUPPORTED.
 * @note Owner thread, no execution; matching destroy releases all storage.
 */
typedef cog_result_t (*cog_tool_async_new_fn)(cog_core_t *core,
    const char *request, size_t request_len,
    const cog_tool_async_config_t *config, void **out_handle);
/**
 * @brief Begin effects once after policy and persistence; see table contract.
 * @param[in,out] handle Prepared tool state.
 * @param[in] now_ms Monotonic time.
 * @return OK, NULL_PTR, INVALID_ARG or INVALID_STATE; failure has no effects.
 * @note Owner thread, nonblocking; effects begun imply OK and retained outcome.
 */
typedef cog_result_t (*cog_tool_async_start_fn)(void *handle, uint64_t now_ms);
/**
 * @brief Service bounded execution/cleanup work.
 * @param[in,out] handle Admitted tool state.
 * @param[in] now_ms Nondecreasing monotonic time.
 * @param[out] out_done True only when terminal and all owned children reaped.
 * @param[out] out_next_deadline_ms Absolute timer or UINT64_MAX.
 * @return OK, NULL_PTR, INVALID_ARG, INVALID_STATE or fatal driver error.
 * @note Owner thread, no sleeping; result status describes execution failure.
 */
typedef cog_result_t (*cog_tool_async_poll_fn)(void *handle, uint64_t now_ms,
    bool *out_done, uint64_t *out_next_deadline_ms);
/**
 * @brief Request idempotent cancellation without waiting for process exit.
 * @param[in,out] handle Tool state.
 * @param[in] now_ms Nondecreasing monotonic time.
 * @return OK, NULL_PTR, INVALID_ARG. Owner thread; continue poll until done.
 */
typedef cog_result_t (*cog_tool_async_cancel_fn)(void *handle, uint64_t now_ms);
/**
 * @brief View retained terminal output without execution or allocation.
 * @param[in] handle Tool state.
 * @param[out] out_result Borrowed view, unchanged on failure.
 * @return OK, NULL_PTR, AGAIN before done. Owner thread; view until destroy.
 */
typedef cog_result_t (*cog_tool_async_result_fn)(const void *handle,
    cog_tool_output_t *out_result);
/**
 * @brief Free prepared/terminal tool state.
 * @param[in,out] handle Tool state, NULL is OK.
 * @return OK, INVALID_STATE if active (handle remains intact).
 * @note Owner thread; cancel/poll before destruction; no hidden waits.
 */
typedef cog_result_t (*cog_tool_async_destroy_fn)(void *handle);
/** @brief Every callback is required; definitions are static const. */
typedef struct cog_tool_async_vtable {
    cog_tool_async_new_fn new_;
    cog_tool_async_start_fn start;
    cog_tool_async_poll_fn poll;
    cog_tool_async_cancel_fn cancel;
    cog_tool_async_result_fn result;
    cog_tool_async_destroy_fn destroy;
} cog_tool_async_vtable_t;

/** @brief Explicit opt-in capabilities of an asynchronous descriptor. */
typedef enum cog_tool_flag {
    COG_TOOL_REENTRANT = 1u,
    COG_TOOL_INDEPENDENT = 2u,
    COG_TOOL_SIDE_EFFECTING = 4u
} cog_tool_flag_t;

/**
 * @brief Descriptor for a single tool.
 *
 * Rows are assembled into a static const array and supplied to the core
 * through @c cog_core_config_t.
 */
typedef struct cog_tool {
    const char *name;        /**< Tool name used for dispatch.  Non-NULL,
                                  NUL-terminated, unique within the table. */
    const char *description; /**< Human-readable description; may be NULL. */
    const char *input_schema; /**< Input validation schema (e.g. JSON Schema);
                                  may be NULL. */
    cog_tool_fn handler;     /**< Legacy blocking handler; NULL if async only. */
    const cog_tool_async_vtable_t *async; /**< Static operations for responsive use. */
    uint32_t flags; /**< cog_tool_flag_t bits; concurrency requires first two. */
} cog_tool_t;

/**
 * @brief Static tool table wrapper.
 *
 * This is a convenience type for passing a pointer/count pair.  The
 * underlying array must remain valid for the lifetime of any core that
 * references it.
 */
typedef struct cog_tool_table {
    const cog_tool_t *tools; /**< Pointer to first tool descriptor. */
    size_t count;            /**< Number of descriptors.  May be zero. */
} cog_tool_table_t;

/**
 * @brief Look up a tool by name in a tool table.
 *
 * @param[in]  table      Tool table to search.
 * @param[in]  name       Tool name to look up; NUL-terminated.
 * @param[out] out_tool   Receives a pointer to the matching descriptor on
 *                        success; unchanged on failure.  May be NULL if
 *                        only presence is required.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p table or @p name is NULL.
 * @return COG_ERROR_NOT_FOUND if no tool matches @p name.
 *
 * Threading: the table must not be modified during the call.
 */
cog_result_t cog_tool_table_lookup(const cog_tool_table_t *table,
                                   const char *name,
                                   const cog_tool_t **out_tool);

#ifdef __cplusplus
}
#endif

#endif /* COG_TOOL_H */
