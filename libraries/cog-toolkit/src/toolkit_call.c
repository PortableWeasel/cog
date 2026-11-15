/**
 * @file toolkit_call.c
 * @brief Implementation of the tool call context contract declared in
 *        cog/toolkit_call.h.
 *
 * A @c cog_toolkit_call_t is a single pending tool invocation: the tool
 * name, a borrowed request payload, an optional per-call timeout, and a
 * cooperative cancellation flag.  The context does not own the request
 * payload; the caller must keep it alive until execution completes.
 *
 * All storage owned by the context is allocated in a single heap block by
 * the constructor (the struct plus a NUL-terminated copy of the name) and
 * released by the destructor.  The request payload is borrowed, never
 * copied, and never freed here.
 *
 * The module holds no global mutable state.
 *
 * Ticket: 8d52c1a (Stage 12.4b — Implement toolkit)
 */

#include "cog/toolkit_call.h"
#include "toolkit_internal.h"

#include <stdlib.h>
#include <string.h>

/**
 * @brief Maximum timeout accepted by @c cog_toolkit_call_set_timeout.
 *
 * Values above this are "unreasonably large" and rejected with
 * @c COG_ERROR_INVALID_ARG.  2^31-1 ms is roughly 24.8 days, comfortably
 * below any 32-bit tick overflow while far beyond any realistic tool
 * budget.  This is a documented implementation limit.
 */
#define COG_TOOLKIT_TIMEOUT_MAX_MS 0x7FFFFFFFu

/**
 * @brief Private layout of a tool call context.
 *
 * @p name points into the same heap block as the struct (the name copy
 * follows the struct in memory).  @p request is borrowed from the caller.
 */
struct cog_toolkit_call {
    char *name;          /**< NUL-terminated; points into the same block. */
    const char *request; /**< Borrowed request payload; caller owns it. */
    size_t request_len;  /**< Length of @p request in bytes. */
    uint32_t timeout_ms; /**< Per-call timeout; zero means "use default". */
    bool cancelled;      /**< Cooperative cancellation flag. */
};

cog_result_t cog_toolkit_call_new(cog_toolkit_call_t **out,
                                  const char *name,
                                  const char *request,
                                  size_t request_len)
{
    struct cog_toolkit_call *call;
    size_t name_len;
    char *name_storage;

    if (out == NULL || name == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    *out = NULL;

    if (request == NULL && request_len != 0U) {
        return COG_ERROR_INVALID_ARG;
    }

    name_len = strlen(name);

    /* Single heap block: the struct followed by the NUL-terminated name
     * copy.  The request payload is borrowed and not copied. */
    call = (struct cog_toolkit_call *)malloc(sizeof(*call) + name_len + 1U);
    if (call == NULL) {
        return COG_ERROR_NOMEM;
    }
    name_storage = (char *)(call + 1);
    (void)memcpy(name_storage, name, name_len + 1U);

    call->name = name_storage;
    call->request = request;
    call->request_len = request_len;
    call->timeout_ms = 0U;
    call->cancelled = false;

    *out = call;
    return COG_OK;
}

void cog_toolkit_call_free(cog_toolkit_call_t *call)
{
    /* The request payload is borrowed; only the single heap block is
     * released. */
    free(call);
}

cog_result_t cog_toolkit_call_set_timeout(cog_toolkit_call_t *call,
                                          uint32_t timeout_ms)
{
    if (call == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    if (timeout_ms > COG_TOOLKIT_TIMEOUT_MAX_MS) {
        return COG_ERROR_INVALID_ARG;
    }
    call->timeout_ms = timeout_ms;
    return COG_OK;
}

cog_result_t cog_toolkit_call_cancel(cog_toolkit_call_t *call)
{
    if (call == NULL) {
        return COG_ERROR_NULL_PTR;
    }
    call->cancelled = true;
    return COG_OK;
}

bool cog_toolkit_call_is_cancelled(const cog_toolkit_call_t *call)
{
    return call != NULL && call->cancelled;
}

const char *cog_toolkit_call_name(const cog_toolkit_call_t *call)
{
    return call != NULL ? call->name : NULL;
}

const char *cog_toolkit_call_request(const cog_toolkit_call_t *call,
                                     size_t *out_len)
{
    if (call == NULL) {
        if (out_len != NULL) {
            *out_len = 0U;
        }
        return NULL;
    }
    if (out_len != NULL) {
        *out_len = call->request_len;
    }
    if (call->request_len == 0U) {
        return NULL;
    }
    return call->request;
}

uint32_t cog_toolkit_call_timeout_ms(const cog_toolkit_call_t *call)
{
    return call != NULL ? call->timeout_ms : 0U;
}
