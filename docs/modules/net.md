# cog-net {#page_cog_net}

## What it does

`cog-net` is a nonblocking HTTP client for provider traffic. It provides:

- **explicit request construction** with copied inputs,
- **bounded transfer slots** and per-request state,
- **retained chunked responses** that the caller acknowledges in order,
- **stall detection** and sanitized transport error classification,
- a **deadline** the host loop can sleep on.

It does not retry. Retry budgets and backoff belong to the application, so a
request is never replayed behind your back.

Headers: `net.h`.

## Key ideas

**The host owns the dependency lifecycle.** `cog_net_new` requires
`host_runtime_initialized = true` and an explicit acknowledgement that the
underlying library allocates dynamically
(`dependency_memory = COG_NET_DEPENDENCY_MEMORY_DYNAMIC`). The host calls
`curl_global_init` before creating the context and `curl_global_cleanup`
after freeing it.

**Requests are copied.** After `cog_net_request` returns, the caller may free
the method, URL, headers, and body.

**Responses are peeked, then acknowledged.** `cog_net_peek` (or
`cog_net_peek_request` for one id) returns a borrowed, immutable view.
Nothing advances until `cog_net_ack` consumes that exact sequence. A view
stays valid across other service calls.

**HTTP error statuses are responses, not transport failures.** A 429 or 500
is delivered with `http_status` set and `outcome == COG_OK`. Transport
failures carry a `cog_net_error_kind_t` and a sanitized message with no
credentials, URL userinfo, or body content.

**`service` advances everything.** `cog_net_service(ctx, now_ms, &deadline)`
drives framing and completion within a bounded quantum. It never waits.

## Getting started: a complete GET

```c
#include <cog/net.h>

#include <stdint.h>
#include <stdio.h>

/*
 * The host calls curl_global_init() before creating the context and
 * curl_global_cleanup() after cog_net_free().
 */
int main(void)
{
    cog_net_config_t config = {0};
    cog_net_ctx_t *net = NULL;
    cog_net_response_t response;
    cog_request_id_t id = 0;
    uint64_t deadline = 0;
    uint64_t now = 0;
    int status = 1;

    config.host_runtime_initialized = true;
    config.dependency_memory = COG_NET_DEPENDENCY_MEMORY_DYNAMIC;

    if (cog_net_new(&net, &config) != COG_OK) {
        return 1;
    }

    if (cog_net_request(net, "GET", "https://example.com/", NULL, NULL, 0,
                        &id) != COG_OK) {
        cog_net_free(net);
        return 1;
    }

    for (;;) {
        cog_result_t rc;

        (void)cog_net_service(net, now, &deadline);
        rc = cog_net_peek_request(net, id, &response);

        if (rc == COG_ERROR_AGAIN) {
            if (deadline == UINT64_MAX) {
                break; /* nothing pending and no timer */
            }
            now = deadline;
            continue;
        }
        if (rc != COG_OK) {
            break;
        }

        if (response.chunk != NULL) {
            (void)fwrite(response.chunk, 1, response.chunk_len, stdout);
        }

        (void)cog_net_ack(net, id, response.sequence);

        if (response.is_complete) {
            status = (response.outcome == COG_OK) ? 0 : 1;
            break;
        }
    }

    (void)cog_net_request_release(net, id);
    cog_net_free(net);
    return status;
}
```

## Reading the response

`cog_net_response_t` has two lifetimes to keep straight:

- **The view** (`chunk`, `headers`, `error_message`) is context-owned and
  immutable until you acknowledge the sequence or release the request.
- **The sequence** starts at one per request and must be acknowledged in
  order. Acknowledging a stale sequence fails with `COG_ERROR_INVALID_STATE`
  and consumes nothing.

`retry_after_ms` / `retry_after_known` are parsed from the header only when
valid; zero with `retry_after_known == false` means the server said nothing
useful.

## Example: a bounded request

```c
#include <cog/net.h>

cog_result_t start_post(cog_net_ctx_t *net, cog_request_id_t *out_id);

cog_result_t start_post(cog_net_ctx_t *net, cog_request_id_t *out_id)
{
    static const char body[] = "{\"prompt\":\"hello\"}";
    static const char headers[] =
        "content-type: application/json\r\n"
        "authorization: Bearer example";

    return cog_net_request(net, "POST", "https://example.com/v1/chat",
                           headers, body, sizeof body - 1U, out_id);
}
```

Request bodies and headers are copied into constructor-reserved storage.
Exceeding `max_request_bytes` returns `COG_ERROR_LIMIT` atomically: nothing is
sent and no slot is retained.

## Beyond the basics

- **Slots.** `max_connections` bounds concurrent transfers. A slot is reused
  only after `cog_net_request_release`.
- **Stall detection.** `stall_timeout_ms` is byte inactivity *after* the
  response starts. A stalled transfer terminates with
  `COG_NET_ERROR_TIMEOUT` rather than hanging the loop.
- **Multiple consumers.** Providers sharing one context must use
  `cog_net_peek_request` with their own id, never the global fair selector.
- **Cancellation.** `cog_net_cancel` stops wire activity, preserves already
  queued bytes, and reserves exactly one terminal `ABORTED`.

## See also

- @ref page_cog_provider — the real adapter built on this module
- @ref page_cog_obs — connection diagnostics and stall reporting
- `pocs/http-streaming` — the original streaming proof of concept
- `docs/research/https-lifecycle.md` — the transport lifecycle research
