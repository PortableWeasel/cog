/**
 * @file sse.h
 * @brief Incremental SSE (text/event-stream) framing parser for the
 *        HTTP streaming PoC.
 *
 * Server-Sent Events use a line-based framing on top of the HTTP body
 * (RFC 9126): fields are lines of the form "name: value", and a blank
 * line terminates an event. This PoC only cares about `data:` fields; all
 * other fields (event:, id:, retry:) and comments are ignored.
 *
 * The parser is incremental: feed it whatever byte slices the transport
 * delivers (which may split or merge events boundaries arbitrarily) and it
 * invokes the event callback once per complete event.
 */

#ifndef COG_POC_HTTP_SSE_H
#define COG_POC_HTTP_SSE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback invoked for each complete SSE event.
 *
 * @param[in] data NUL-terminated event payload (the `data:` value).
 * @param[in] len  Payload length in bytes (excluding the NUL).
 * @param[in] ud   Opaque user data from the feed/finish call.
 */
typedef void (*sse_event_fn)(const char *data, size_t len, void *ud);

/** Opaque SSE parser; created with sse_parser_create(). */
typedef struct sse_parser sse_parser_t;

/**
 * @brief Create an SSE parser.
 *
 * @param[in] max_event Maximum allowed size of one event payload in bytes.
 *                      0 selects a default (4096). Oversized events are
 *                      truncated to this limit.
 * @return Pointer to the parser, or NULL on allocation failure.
 */
sse_parser_t *sse_parser_create(size_t max_event);

/**
 * @brief Destroy a parser created by sse_parser_create().
 *
 * @param[in,out] p Parser to destroy (may be NULL).
 */
void sse_parser_destroy(sse_parser_t *p);

/**
 * @brief Feed a slice of response body bytes into the parser.
 *
 * Complete events are dispatched immediately via @p cb. Partial lines and
 * events are buffered until the rest of the bytes arrive.
 *
 * @param[in,out] p    Parser to feed.
 * @param[in]     data Body bytes.
 * @param[in]     len  Number of bytes in @p data.
 * @param[in]     cb   Event callback (may be NULL).
 * @param[in]     ud   User data forwarded to @p cb.
 */
void sse_parser_feed(sse_parser_t *p, const uint8_t *data, size_t len,
                     sse_event_fn cb, void *ud);

/**
 * @brief Flush any pending event at end of stream.
 *
 * If the body ends without a terminating blank line, the accumulated
 * `data:` payload is emitted as one last event.
 *
 * @param[in,out] p  Parser to flush.
 * @param[in]     cb Event callback (may be NULL).
 * @param[in]     ud User data forwarded to @p cb.
 */
void sse_parser_finish(sse_parser_t *p, sse_event_fn cb, void *ud);

#ifdef __cplusplus
}
#endif

#endif /* COG_POC_HTTP_SSE_H */
