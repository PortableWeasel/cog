/**
 * @file http_common.h
 * @brief Shared API for the two HTTP transports in the streaming PoC.
 *
 * Both the hand-rolled client (http_hand.c) and the libcurl client
 * (http_curl.c) expose the same request/result contract so the demo can
 * compare them side by side:
 *
 *   - The caller provides a URL, a config, a connection tracker, and an
 *     optional chunk callback.
 *   - The transport is responsible for HTTP/1.1 framing only: it strips
 *     Transfer-Encoding: chunked and hands body bytes to the chunk
 *     callback. SSE event framing is layered on top by the demo using
 *     sse.h.
 *   - The transport updates the tracker (bytes, chunks, state) and runs
 *     stall detection; the result describes how the transfer ended.
 */
#ifndef COG_POC_HTTP_COMMON_H
#define COG_POC_HTTP_COMMON_H

#include "conn.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Outcome classification for a request or transfer. */
typedef enum http_err {
    HTTP_ERR_NONE = 0, /**< Transfer completed successfully. */
    HTTP_ERR_RETRYABLE, /**< Server replied 5xx; can be retried. */
    HTTP_ERR_CLIENT,    /**< Server replied 4xx; do not retry. */
    HTTP_ERR_STALL,     /**< No chunk arrived within the stall threshold. */
    HTTP_ERR_NET,       /**< Socket connect/read/write failure or EOF. */
    HTTP_ERR_PARSE,     /**< Malformed HTTP response. */
    HTTP_ERR_TIMEOUT    /**< Overall time budget exhausted. */
} http_err_t;

/** Behaviour knobs shared by both transports. */
typedef struct http_client_cfg {
    uint64_t stall_ms;        /**< Stall threshold in milliseconds. */
    unsigned max_retries;     /**< Max retries on transient 5xx responses. */
    uint64_t overall_timeout_ms; /**< Total budget for the whole request. */
} http_client_cfg_t;

/**
 * @brief Callback receiving decoded body bytes.
 *
 * @param[in] data Body bytes (may be any slice size; framing is not
 *                 guaranteed to align with SSE events).
 * @param[in] len  Byte count.
 * @param[in] ud   User data from http_request_t::user_data.
 */
typedef void (*http_chunk_fn)(const uint8_t *data, size_t len, void *ud);

/**
 * @brief Optional live status callback while a transfer is streaming.
 *
 * The libcurl transport (multi interface) calls this periodically while
 * the transfer is in flight. The hand-rolled transport calls it between
 * chunks. Both pass the current connection-tracker snapshot, giving the
 * demo a live view of Observability data.
 *
 * @param[in] t  Tracker snapshot source.
 * @param[in] ud User data from http_request_t::user_data.
 */
typedef void (*http_status_fn)(const conn_tracker_t *t, void *ud);

/** One request execution context (caller-allocated, stack-friendly). */
typedef struct http_request {
    const char *url;            /**< Full http:// URL. */
    const http_client_cfg_t *cfg; /**< Behaviour knobs (required). */
    conn_tracker_t *tracker;    /**< Tracks this request (required). */
    http_chunk_fn on_chunk;     /**< May be NULL. */
    http_status_fn on_status;   /**< May be NULL. */
    void *user_data;            /**< Forwarded to both callbacks. */
} http_request_t;

/** Outcome of one request, including retries. */
typedef struct http_result {
    http_err_t err;    /**< Outcome classification. */
    int status;        /**< Last HTTP status code seen. */
    unsigned retries;  /**< Number of 5xx retries performed. */
    size_t bytes;      /**< Body bytes consumed (last attempt). */
    size_t chunks;     /**< Chunks delivered to the chunk callback. */
    conn_state_t state; /**< Final tracker state. */
} http_result_t;

/**
 * @brief Run a request with the hand-rolled HTTP/1.1 client.
 *
 * A minimal, dependency-free implementation of GET over TCP with chunked
 * transfer-encoding decoding, poll()-based stall detection, and 5xx retry.
 * See pocs/http-streaming/README.md for the design rationale.
 *
 * @param[in] req Request context (all fields except callbacks required).
 * @return Result describing the outcome.
 */
http_result_t hand_request(const http_request_t *req);

/**
 * @brief Run a request with the libcurl client (multi interface).
 *
 * Uses curl_multi so the caller's status callback fires while the
 * transfer streams, and the loop can enforce stall detection via the
 * connection tracker. Requires libcurl >= 7.66 for curl_multi_poll.
 *
 * @param[in] req Request context (all fields except callbacks required).
 * @return Result describing the outcome.
 */
http_result_t curl_request(const http_request_t *req);

/**
 * @brief Human-readable name for an http_err_t value.
 *
 * @param[in] e Error code to name.
 * @return Static string (never NULL).
 */
const char *http_err_name(http_err_t e);

/**
 * @brief Split an "http://host[:port]/path" URL into its parts.
 *
 * @param[in]  url      URL to split (must start with "http://").
 * @param[out] host     Host buffer.
 * @param[in]  host_cap Host buffer capacity.
 * @param[out] port     Port (defaults to 80 when omitted).
 * @param[out] path     Path (defaults to "/"); points into @p url.
 * @return 0 on success, -1 on parse failure.
 */
int http_split_url(const char *url, char *host, size_t host_cap,
                   uint16_t *port, const char **path);

#ifdef __cplusplus
}
#endif

#endif /* COG_POC_HTTP_COMMON_H */
