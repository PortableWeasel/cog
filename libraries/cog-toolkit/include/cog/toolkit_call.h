/**
 * @file toolkit_call.h
 * @brief Tool call context for a single toolkit invocation.
 *
 * A @c cog_toolkit_call_t represents one pending tool execution: the tool
 * name, the serialised request payload, an optional timeout, and a
 * cancellation flag.  Callers create the context, configure it, pass it
 * to @c cog_toolkit_execute or @c cog_toolkit_execute_all, and then free
 * it. The constructor copies the name and request and reserves 64 KiB of
 * legacy result storage. Its runtime paths never grow storage or repeat a
 * handler after overflow. Responsive clients use an execution handle instead.
 *
 * Threading model: a single @c cog_toolkit_call_t instance is not
 * thread-safe.  It should be configured on the same thread that will pass
 * it to the execution dispatcher.
 */
#ifndef COG_TOOLKIT_CALL_H
#define COG_TOOLKIT_CALL_H

#include <cog/result.h>
#include <cog/tool.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration to break the core/toolkit include cycle. */
typedef struct cog_core cog_core_t;

/**
 * @brief Opaque context for a single tool call.
 *
 * Created with @c cog_toolkit_call_new and destroyed with
 * @c cog_toolkit_call_free.  All storage is allocated in a single heap
 * block by the constructor and released by the destructor.
 */
typedef struct cog_toolkit_call cog_toolkit_call_t;

/**
 * @brief Create a new tool call context.
 *
 * @param[out] out        Uninitialised pointer; set to the new context on
 *                        success.
 * @param[in]  name       Tool name to dispatch; NUL-terminated and must
 *                        match an entry in the tool table supplied at
 *                        execution time.
 * @param[in]  request    Serialised request payload (e.g. JSON arguments);
 *                        may be NULL when @p request_len is zero.
 * @param[in]  request_len Length of @p request in bytes.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p out or @p name is NULL.
 * @return COG_ERROR_INVALID_ARG if @p request is NULL and
 *         @p request_len is non-zero.
 * @return COG_ERROR_NOMEM if allocation fails.
 *
 * @post On success, @p *out owns copied name/request and fixed result storage;
 * caller input storage may immediately be freed. One call context is one
 * invocation; every later execute copies the cached outcome, never re-executes.
 * @post On failure, @p *out is NULL and no resources are held.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_toolkit_call_new(cog_toolkit_call_t **out,
                                  const char *name,
                                  const char *request,
                                  size_t request_len);

/**
 * @brief Destroy a tool call context.
 *
 * @param[in,out] call Context to destroy; may be NULL (no-op).
 *
 * @post All storage owned by @p call is released.  @p call must not be
 *       used after this call.  The request payload supplied at
 *       construction was copied, so caller storage remains caller-owned.
 *
 * Threading: not thread-safe.
 */
void cog_toolkit_call_free(cog_toolkit_call_t *call);

/**
 * @brief Set the wall-clock timeout for this tool call.
 *
 * A value of zero disables the explicit timeout and lets the execution
 * dispatcher apply its own default.  The timeout is advisory: the
 * underlying tool implementation is responsible for checking cancellation
 * and honouring the deadline.
 *
 * @param[in,out] call       Call context to configure.
 * @param[in]     timeout_ms Timeout in milliseconds; zero means "use the
 *                           dispatcher default".
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p call is NULL.
 * @return COG_ERROR_INVALID_ARG if @p timeout_ms is unreasonably large
 *         (e.g. would overflow internal tick calculations).
 *
 * @post On success, @p call records the requested timeout.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_toolkit_call_set_timeout(cog_toolkit_call_t *call,
                                          uint32_t timeout_ms);

/**
 * @brief Request cancellation of a pending or running tool call.
 *
 * Cancellation is cooperative.  The flag is set immediately; the tool
 * implementation and the execution dispatcher observe the flag at their
 * next cancellation point and return @c COG_ERROR_ABORTED.  Calling this
 * function on a call that has already finished has no effect.
 *
 * @param[in,out] call Call context to cancel.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p call is NULL.
 *
 * @post The cancellation flag in @p call is set.
 *
 * Threading: not thread-safe.  In a multi-threaded program the caller
 * must ensure that no other thread is concurrently reading or executing
 * @p call.  In particular, do not cancel a call from one thread while
 * @c cog_toolkit_execute_all is running it on another unless both sides
 * are synchronised.
 */
cog_result_t cog_toolkit_call_cancel(cog_toolkit_call_t *call);

/**
 * @brief Return true if the call has been cancelled.
 *
 * @param[in] call Call context to query; may be NULL.
 * @return true if @p call is non-NULL and its cancellation flag is set,
 *         false otherwise.
 *
 * Threading: not thread-safe.
 */
bool cog_toolkit_call_is_cancelled(const cog_toolkit_call_t *call);

/**
 * @brief Return the tool name recorded in the call context.
 *
 * @param[in] call Call context to query; may be NULL.
 * @return Pointer to a NUL-terminated name string, or NULL if @p call is
 *         NULL.
 *
 * @note The returned pointer remains valid until the call context is
 *       freed.
 *
 * Threading: not thread-safe.
 */
const char *cog_toolkit_call_name(const cog_toolkit_call_t *call);

/**
 * @brief Return the request payload recorded in the call context.
 *
 * @param[in]  call       Call context to query; may be NULL.
 * @param[out] out_len    Receives the length of the request in bytes; may
 *                        be NULL if the caller does not need the length.
 * @return Pointer to the request payload, or NULL if @p call is NULL or
 *         the request length is zero.
 *
 * @note The returned pointer remains valid until the call context is
 *       freed; the returned copied payload is owned by the call.
 *
 * Threading: not thread-safe.
 */
const char *cog_toolkit_call_request(const cog_toolkit_call_t *call,
                                     size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* COG_TOOLKIT_CALL_H */
