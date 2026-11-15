# net.h Contract

`libraries/cog-net/include/cog/net.h` — HTTP client with streaming, stall
detection, and retry.

This document records the executable contract verified by
`tests/unit/test_net.c`. It intentionally does **not** expand the public
API; it only documents what the header already declares and how the
implementation (`libraries/cog-net/src/net.c`) satisfies it.

## Status

Implemented. `cog-net` is a static library compiling `src/net.c`. Guard:
`COG_NET_H`.

## Public types

| Type | Kind | Notes |
|------|------|-------|
| `cog_net_ctx_t` | opaque struct | Created by `cog_net_new`, destroyed by `cog_net_free`. |
| `cog_request_id_t` | `uint64_t` | Assigned by `cog_net_request`; used for tracking/cancel/observability. |
| `cog_net_config_t` | struct | Behaviour knobs (see below). |
| `cog_net_response_t` | struct | One response chunk from `cog_net_poll`. |

### `cog_net_config_t` fields

| Field | Meaning | Default when NULL / zero |
|-------|---------|--------------------------|
| `max_connections` | Max concurrent connections; 0 selects a default. | 16 |
| `connect_timeout_ms` | TCP connect timeout; 0 selects a default. | 5000 |
| `stall_timeout_ms` | Abort if no data received for this long; 0 selects a default. | 30000 |
| `max_retries` | Max retries on transient failure. | 2 |
| `retry_backoff_ms` | Base backoff between retries. | 200 |

### `cog_net_response_t` fields

| Field | Meaning |
|-------|---------|
| `http_status` | HTTP status code (200, 429, 500, ...). |
| `headers` | Newline-separated `key:value` response headers, or NULL. |
| `chunk` | Body chunk (may be a partial/streaming chunk), or NULL. |
| `chunk_len` | Length of `chunk` in bytes. |
| `is_complete` | True if this is the final chunk for the request. |
| `request_id` | Request this response belongs to. |

## Functions

| Function | Returns |
|----------|---------|
| `cog_net_new` | `COG_OK`, `COG_ERROR_NULL_PTR`, `COG_ERROR_NOMEM`, `COG_ERROR_IO` |
| `cog_net_free` | void |
| `cog_net_request` | `COG_OK`, `COG_ERROR_NULL_PTR`, `COG_ERROR_OVERFLOW`, `COG_ERROR_NOMEM`, `COG_ERROR_INVALID_ARG` |
| `cog_net_cancel` | `COG_OK`, `COG_ERROR_NULL_PTR`, `COG_ERROR_NOT_FOUND` |
| `cog_net_poll` | `COG_OK`, `COG_ERROR_NULL_PTR`, `COG_ERROR_AGAIN`, `COG_ERROR_ABORTED`, `COG_ERROR_TIMEOUT`, `COG_ERROR_IO`, `COG_ERROR_NOMEM` |
| `cog_net_fd` | fd, or -1 |
| `cog_net_active_count` | size_t |
| `cog_net_active_ids` | `COG_OK`, `COG_ERROR_NULL_PTR`, `COG_ERROR_OVERFLOW` |
| `cog_net_request_status` | `COG_OK`, `COG_ERROR_NULL_PTR`, `COG_ERROR_NOT_FOUND` |

## Error semantics

- `cog_net_poll` returns `COG_OK` with a populated `cog_net_response_t`
  whenever a chunk (headers, body, or the terminal `is_complete` node) is
  available. The returned `headers`/`chunk` pointers are valid until the
  next call to `cog_net_poll`.
- `COG_ERROR_AGAIN` means no response is currently available; the caller
  should retry later (the request is still in flight).
- `COG_ERROR_ABORTED` is returned when a cancelled request's terminal node
  is drained; `out_response.request_id` identifies the cancelled request
  and `is_complete` is true.
- `COG_ERROR_TIMEOUT` is returned when a request exhausts its retry budget
  after a stall or connect timeout; `out_response.request_id` identifies
  the request.
- `COG_ERROR_IO` is returned when a request fails terminally (network
  error, malformed response, connection refused) with no retry budget
  remaining; `out_response.request_id` identifies the request.
- `COG_ERROR_NOMEM` is returned if an internal allocation fails.
- `cog_net_cancel` returns `COG_ERROR_NOT_FOUND` if the request does not
  exist (including after it has already been cancelled or completed).

## Retry / backoff / stall semantics

- Transient failures (connect failure, network error, stall, and HTTP
  `5xx`) are retried autonomously up to `max_retries`, with a
  `retry_backoff_ms` delay between attempts. The caller observes only the
  eventual outcome.
- A stall is detected when no data arrives within `stall_timeout_ms`
  while a request is connecting, sending, or reading. If retries remain,
  the request reconnects; otherwise it fails with `COG_ERROR_TIMEOUT`.
- HTTP `4xx` (including `429`) responses are surfaced to the caller as a
  normal completed response carrying `http_status` and the response
  headers (e.g. `Retry-After`). The application decides whether/how to
  queue or retry a rate-limited request; cog-net does not queue at the
  provider level.
- After `max_retries` are exhausted on a `5xx`, the final `5xx` response
  is surfaced to the caller with its `http_status`.

## Threading

Single-threaded per context. `cog_net_poll` is the only driver of
progress; it performs non-blocking socket I/O and must be called from the
core's event loop. `cog_net_fd` returns a wakeup fd for external
`poll()`/`epoll()` loops. No function is safe to call concurrently from
another thread.

## Ownership

- `cog_net_ctx_t` is caller-allocated by `cog_net_new` and released by
  `cog_net_free`. `cog_net_free` cancels all in-flight requests and
  releases all resources.
- Request method/url/headers/body are copied by `cog_net_request`; the
  caller's buffers need not outlive the call.
- Response `headers`/`chunk` pointers are owned by the context and valid
  only until the next `cog_net_poll`.
- There is no global mutable state; every allocation is owned by the
  context and freed by `cog_net_free`.

## Invariants

- Request ids are unique and non-zero within a context.
- A request slot is released (and `active_count` decremented) once its
  terminal node is enqueued; a completed or cancelled request is no longer
  found by `cog_net_request_status` or `cog_net_cancel`.
- `cog_net_active_ids` reports exactly the in-flight request ids; a
  too-small buffer returns `COG_ERROR_OVERFLOW`.
- Partial socket I/O is handled: sends loop until all bytes are written;
  reads buffer over-read bytes so line/head parsing never loses data.

## Verification

`tests/unit/test_net.c` drives the API against a local loopback test
server (fork-per-connection, no external network) and pins:

- lifecycle, defaults, and NULL-argument validation
- request ids and `max_connections` overflow
- request/stream completion, response headers and status
- fragmented headers and chunks (partial socket I/O)
- request body round-trip (Content-Length) and read-until-close
- cancellation and bulk active-id queries
- stall detection (`COG_ERROR_TIMEOUT`) and stall recovery via retry
- retry/backoff on `5xx`, and `5xx` surfacing when retries are exhausted
- `429` rate-limit metadata (`Retry-After` header)
- malformed responses (`COG_ERROR_IO`)
- connection refused (`COG_ERROR_IO`)
- cleanup cancelling in-flight work

Run with `ctest -R unit-net` (plain, ASan/UBSan, and Clang builds all
pass).