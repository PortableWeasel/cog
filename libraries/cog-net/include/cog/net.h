/**
 * @file net.h
 * @brief HTTP client with streaming, stall detection, and retry.
 *
 * Each core process creates its own network context.  Connections are
 * tracked per-context with request ids for observability.  The provider
 * registry uses cog-net to make LLM API calls.
 *
 * Threading model: single-threaded per context.  The poll-based event
 * loop integrates with the core's run loop via cog_net_poll().
 */
#ifndef COG_NET_H
#define COG_NET_H

#include <cog/result.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque network context.
 *
 * Created with cog_net_new and destroyed with cog_net_free.
 * Each core process typically owns one context.
 */
typedef struct cog_net_ctx cog_net_ctx_t;

/**
 * @brief Unique request identifier.
 *
 * Assigned by cog_net_request.  Used for tracking, cancellation,
 * and observability.
 */
typedef uint64_t cog_request_id_t;

/**
 * @brief Configuration for a network context.
 */
typedef struct cog_net_config {
    /** Maximum concurrent connections.  Zero selects a default. */
    size_t max_connections;

    /** TCP connect timeout in milliseconds. */
    uint32_t connect_timeout_ms;

    /** Stall detection: abort if no data received for this long. */
    uint32_t stall_timeout_ms;

    /** Maximum number of retries on transient failure. */
    size_t max_retries;

    /** Base backoff delay between retries in milliseconds. */
    uint32_t retry_backoff_ms;
} cog_net_config_t;

/**
 * @brief Response chunk from cog_net_poll.
 */
typedef struct cog_net_response {
    /** HTTP status code (e.g. 200, 429, 500). */
    int http_status;

    /** Response headers (newline-separated key:value pairs).
     *  May be NULL if no headers are available.  Used for
     *  Retry-After and other metadata. */
    const char *headers;

    /** Response body chunk (may be a partial/streaming chunk). */
    const char *chunk;

    /** Length of the chunk in bytes. */
    size_t chunk_len;

    /** True if this is the final chunk for this request. */
    bool is_complete;

    /** Request id this response belongs to. */
    cog_request_id_t request_id;
} cog_net_response_t;

/* ── Lifecycle ──────────────────────────────────────────────────── */

/**
 * @brief Create a new network context.
 *
 * @param[out] out    Uninitialised pointer; set to the new context.
 * @param[in]  config Configuration; may be NULL for defaults.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p out is NULL.
 * @return COG_ERROR_NOMEM if allocation fails.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_net_new(cog_net_ctx_t **out,
                          const cog_net_config_t *config);

/**
 * @brief Destroy a network context.
 *
 * Cancels all in-flight requests and releases all resources.
 *
 * @param[in,out] ctx Context to destroy; may be NULL (no-op).
 *
 * Threading: not thread-safe.
 */
void cog_net_free(cog_net_ctx_t *ctx);

/* ── Requests ───────────────────────────────────────────────────── */

/**
 * @brief Start an HTTP request.
 *
 * The request is queued and executed asynchronously.  Use cog_net_poll
 * to receive response chunks.
 *
 * @param[in,out] ctx      Network context.
 * @param[in]     method   HTTP method ("GET", "POST", etc.).
 * @param[in]     url      Full URL.
 * @param[in]     headers  Optional headers (newline-separated key:value).
 * @param[in]     body     Request body; may be NULL.
 * @param[in]     body_len Body length in bytes.
 * @param[out]    out_id   Receives the request identifier.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p ctx, @p method, @p url, or @p out_id
 *         is NULL.
 * @return COG_ERROR_OVERFLOW if max_connections is reached.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_net_request(cog_net_ctx_t *ctx,
                              const char *method,
                              const char *url,
                              const char *headers,
                              const char *body, size_t body_len,
                              cog_request_id_t *out_id);

/**
 * @brief Cancel an in-flight request.
 *
 * @param[in,out] ctx Network context.
 * @param[in]     id  Request to cancel.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p ctx is NULL.
 * @return COG_ERROR_NOT_FOUND if the request does not exist.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_net_cancel(cog_net_ctx_t *ctx, cog_request_id_t id);

/* ── Polling ────────────────────────────────────────────────────── */

/**
 * @brief Poll for completed and streaming responses.
 *
 * Call from the core's event loop.  Returns the next available response
 * chunk.  For streaming responses, call repeatedly until is_complete
 * is true.
 *
 * @param[in,out] ctx         Network context.
 * @param[out]    out_response Receives the next response chunk.
 * @return COG_OK if a response chunk is available.
 * @return COG_ERROR_NULL_PTR if @p ctx or @p out_response is NULL.
 * @return COG_ERROR_AGAIN if no response is available.
 * @return COG_ERROR_ABORTED if the request was cancelled.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_net_poll(cog_net_ctx_t *ctx,
                           cog_net_response_t *out_response);

/**
 * @brief Return the fd for use in external poll()/epoll loops.
 *
 * @param[in] ctx Network context.
 * @return File descriptor, or -1 on error.
 *
 * Threading: not thread-safe.
 */
int cog_net_fd(const cog_net_ctx_t *ctx);

/* ── Observability ──────────────────────────────────────────────── */

/**
 * @brief Return the number of active connections.
 *
 * @param[in] ctx Network context; may be NULL.
 * @return Number of active connections, or zero if @p ctx is NULL.
 *
 * Threading: not thread-safe.
 */
size_t cog_net_active_count(const cog_net_ctx_t *ctx);

/**
 * @brief Get the ids of all active requests.
 *
 * Used for bulk cancellation during shutdown.
 *
 * @param[in]  ctx       Network context.
 * @param[out] out_ids   Caller-provided array for request ids.
 * @param[in]  max_ids   Maximum entries in @p out_ids.
 * @param[out] out_count Receives the actual number of active requests.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p ctx, @p out_ids, or @p out_count is NULL.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_net_active_ids(const cog_net_ctx_t *ctx,
                                 cog_request_id_t *out_ids,
                                 size_t max_ids,
                                 size_t *out_count);

/**
 * @brief Query the status of a specific request.
 *
 * @param[in]  ctx        Network context.
 * @param[in]  id         Request to query.
 * @param[out] out_active Receives true if the request is still active.
 * @return COG_OK on success.
 * @return COG_ERROR_NULL_PTR if @p ctx or @p out_active is NULL.
 * @return COG_ERROR_NOT_FOUND if the request does not exist.
 *
 * Threading: not thread-safe.
 */
cog_result_t cog_net_request_status(const cog_net_ctx_t *ctx,
                                     cog_request_id_t id,
                                     bool *out_active);

#ifdef __cplusplus
}
#endif

#endif /* COG_NET_H */