# cog-provider {#page_cog_provider}

## What it does

`cog-provider` is the boundary between Cog and an LLM service. It provides:

- a **static registry** of endpoints, each with a base URL, a credential, a
  vtable, and a table of models,
- the **provider vtable** — nine callbacks that any backend implements,
- **request/response types** with structural validation,
- **preflight** checks that validate a complete request *without* touching
  the network.

The library ships no real network backend of its own. The OpenRouter adapter
(`openrouter.h`) is built on @ref page_cog_net "cog-net", and
`cog_cli/mock_provider.h` supplies a deterministic fixture for tests.

Headers: `provider.h`, `provider_vtable.h`, `provider_request.h`,
`endpoint.h`, `openrouter.h`.

## Key ideas

**The vtable is `static const` and complete.** All nine callbacks —
`connect`, `disconnect`, `send`, `receive`, `poll`, `cancel`, `next`,
`error`, and `preflight` — must be non-NULL. `cog_provider_connect` validates
this before calling the backend.

**`cog_provider_t` is a vtable-carrying base.** A backend's instance type
places `cog_provider_t` as its first member so generic calls can dispatch.

**Requests are copied and validated before send.** `cog_provider_send` owns a
transitive copy of the request; the caller may free its data immediately.
A failed send has no network effect and leaves the instance idle.

**Transport never retries.** Retry budgets belong to the application.
`cog_provider_error_t.replay_safe` tells it whether a retry is even safe (no
output accepted yet, no side effect started).

**Generations, not pointers, identify configuration.** Endpoints carry a
`generation`; requests must agree with it. This makes hot reload race-free.

## Getting started: implement a minimal backend

This example defines a complete (if boring) provider that answers every
request with fixed text. It shows the exact shape of the vtable.

```c
#include <cog/provider.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const cog_provider_vtable_t demo_vtable;
static cog_provider_t demo_instance;

static cog_result_t demo_connect(const cog_endpoint_t *endpoint,
                                 cog_provider_t **out_provider)
{
    (void)endpoint;
    demo_instance.vtable = &demo_vtable;
    *out_provider = &demo_instance;
    return COG_OK;
}

static cog_result_t demo_disconnect(cog_provider_t *provider)
{
    (void)provider;
    return COG_OK;
}

static cog_result_t demo_preflight(cog_provider_t *provider,
                                   const cog_provider_request_t *request,
                                   cog_provider_preflight_stage_t stage)
{
    (void)provider;
    (void)stage;
    return request != NULL ? COG_OK : COG_ERROR_NULL_PTR;
}

static cog_result_t demo_send(cog_provider_t *provider,
                              const cog_provider_request_t *request)
{
    (void)provider;
    (void)request;
    return COG_OK;
}

static cog_result_t demo_receive(cog_provider_t *provider,
                                 cog_provider_response_t *response)
{
    static const char text[] = "hello from the demo provider";
    size_t length = sizeof text - 1U;

    (void)provider;
    if (response->content_capacity < length + 1U) {
        response->content_len = length;
        return COG_ERROR_OVERFLOW;
    }
    memcpy(response->content, text, length + 1U);
    response->content_len = length;
    response->finish_reason = COG_FINISH_REASON_STOP;
    response->is_complete = true;
    return COG_OK;
}

static cog_result_t demo_poll(cog_provider_t *provider, uint64_t now_ms,
                              uint64_t *out_next_deadline_ms)
{
    (void)provider;
    (void)now_ms;
    *out_next_deadline_ms = UINT64_MAX;
    return COG_OK;
}

static cog_result_t demo_cancel(cog_provider_t *provider)
{
    (void)provider;
    return COG_OK;
}

static cog_result_t demo_next(cog_provider_t *provider,
                              cog_provider_delta_t *out_delta,
                              void *storage, size_t capacity,
                              size_t *out_required)
{
    (void)provider;
    (void)out_delta;
    (void)storage;
    (void)capacity;
    *out_required = 0;
    return COG_ERROR_AGAIN; /* this backend only implements receive() */
}

static cog_result_t demo_error(const cog_provider_t *provider,
                               cog_provider_error_t *out_error)
{
    (void)provider;
    (void)out_error;
    return COG_ERROR_NOT_FOUND;
}

static const cog_provider_vtable_t demo_vtable = {
    .connect = demo_connect,
    .disconnect = demo_disconnect,
    .send = demo_send,
    .receive = demo_receive,
    .poll = demo_poll,
    .cancel = demo_cancel,
    .next = demo_next,
    .error = demo_error,
    .preflight = demo_preflight,
};

static const cog_endpoint_t demo_endpoint = {
    .name = "demo",
    .base_url = "https://example.invalid/v1",
    .vtable = &demo_vtable,
    .generation = 1,
};

int main(void)
{
    cog_provider_t *provider = NULL;
    cog_provider_request_t request = {0};
    cog_provider_response_t response = {0};
    char buffer[128];
    cog_result_t rc;

    request.model_id = "demo-model";
    request.max_tokens = 64;

    rc = cog_provider_connect(&demo_endpoint, &provider);
    if (rc != COG_OK) {
        fprintf(stderr, "connect: %d\n", (int)rc);
        return 1;
    }

    if (cog_provider_send(provider, &request) != COG_OK) {
        (void)cog_provider_disconnect(provider);
        return 1;
    }

    response.content = buffer;
    response.content_capacity = sizeof buffer;
    do {
        rc = cog_provider_receive(provider, &response);
    } while (rc == COG_ERROR_AGAIN);

    if (rc == COG_OK) {
        printf("%s\n", buffer);
    }

    (void)cog_provider_disconnect(provider);
    return rc == COG_OK ? 0 : 1;
}
```

## Registry lookup

Endpoints and models are plain static tables. Look them up by name; there is
no registration step and no global state.

```c
#include <cog/endpoint.h>

#include <stddef.h>

static const cog_model_t models[] = {
    { .id = "demo-small", .context_window = 8192,
      .capabilities = COG_MODEL_CAPABILITY_STREAMING },
};

static const cog_model_table_t model_table = { models, 1 };

static const cog_endpoint_t endpoints[] = {
    { .name = "demo", .base_url = "https://example.invalid/v1",
      .vtable = NULL, .models = &model_table, .generation = 1 },
};

static const cog_endpoint_table_t endpoint_table = { endpoints, 1 };

const cog_endpoint_t *find_endpoint(void);

const cog_endpoint_t *find_endpoint(void)
{
    const cog_endpoint_t *endpoint = NULL;

    if (cog_endpoint_table_lookup(&endpoint_table, "demo", &endpoint) != COG_OK) {
        return NULL;
    }
    return endpoint;
}
```

## Using the real OpenRouter adapter

Real use combines this module with `cog-net`:

1. The host initialises the transport dependency (for example
   `curl_global_init`).
2. It creates a `cog_net_ctx_t` and keeps it alive.
3. It fills a `cog_openrouter_config_t` with that context and explicit
   limit reservations.
4. It points an endpoint's `vtable` at the shipped adapter and its
   `implementation_config` at the OpenRouter config.
5. It connects, sends, and then drives `send`/`poll`/`next` from the core's
   service hook.

Credentials are resolved by the application and held by the endpoint for the
lifetime of the connection. They are never logged or persisted.

## Beyond the basics

- **Streaming.** Use `cog_provider_next`, which delivers typed deltas (text,
  tool-begin/arguments/end, opaque continuation, usage, finish) into
  caller-provided storage. `receive` is the legacy text-only path.
- **Cancellation.** `cog_provider_cancel` is idempotent and guarantees exactly
  one terminal `FINISH` event; it never waits for the peer.
- **Error classification.** `cog_provider_get_error` returns a sanitized
  snapshot with HTTP status, retryability, and a stable error kind.
- **Preflight.** `cog_provider_preflight` validates a complete selected
  history (including open tool groups) with no wire effects. Use it before
  resuming a restored session.

## See also

- @ref page_cog_net — the transport the real adapter uses
- @ref page_cog_core — how provider deltas become events
- @ref page_cog_config — endpoint and limit configuration
