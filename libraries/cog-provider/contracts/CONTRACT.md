# cog-provider Contract

`libraries/cog-provider/include/cog/provider.h` — LLM provider abstraction,
endpoint registry, and model capability table.

This document records the executable contract verified by
`tests/unit/test_provider.c`. It intentionally does **not** expand the
public API; it only documents what the headers already declare and how the
implementation (`libraries/cog-provider/src/provider.c` and
`src/endpoint.c`) satisfies it.

## Status

Implemented. `cog-provider` is a static library compiling `provider.c` and
`endpoint.c`. Guards: `COG_PROVIDER_H`, `COG_PROVIDER_REQUEST_H`,
`COG_PROVIDER_VTABLE_H`, `COG_ENDPOINT_H`.

## Public types

| Type | Kind | Notes |
|------|------|-------|
| `cog_provider_request_t` | struct | One LLM call: model id, message history, sampling params, streaming flag, optional tools JSON. |
| `cog_provider_response_t` | struct | Caller-provided content buffer plus finish reason / completion flag. |
| `cog_provider_error_t` | struct | Detailed error: result code, HTTP status, message, `retryable`, `retry_after_ms`. |
| `cog_provider_t` | opaque struct | Provider instance created by connect, destroyed by disconnect. |
| `cog_provider_vtable_t` | struct | Static table of `connect`/`disconnect`/`send`/`receive` function pointers. |
| `cog_endpoint_t` / `cog_endpoint_table_t` | struct | Endpoint descriptor and static table. |
| `cog_model_t` / `cog_model_table_t` | struct | Model descriptor and static table. |
| `cog_model_capability_t` | enum | Streaming / tools / vision capability flags. |
| `cog_finish_reason_t` | enum | Why a response finished (stop, length, tool calls, error, none). |

## Functions

| Function | Returns |
|----------|---------|
| `cog_provider_request_validate` | `COG_OK`, `COG_ERROR_NULL_PTR`, `COG_ERROR_INVALID_ARG` |
| `cog_provider_response_validate` | `COG_OK`, `COG_ERROR_NULL_PTR`, `COG_ERROR_INVALID_ARG` |
| `cog_provider_connect` | `COG_OK`, `COG_ERROR_NULL_PTR`, or the provider's connect result |
| `cog_provider_disconnect` | `COG_OK` (NULL is a no-op), or the provider's disconnect result |
| `cog_provider_send` | `COG_OK`, `COG_ERROR_NULL_PTR`, or the provider's send result |
| `cog_provider_receive` | `COG_OK`, `COG_ERROR_NULL_PTR`, or the provider's receive result |
| `cog_endpoint_table_lookup` | `COG_OK`, `COG_ERROR_NULL_PTR`, `COG_ERROR_NOT_FOUND`, `COG_ERROR_INVALID_ARG` |
| `cog_endpoint_model_lookup` | `COG_OK`, `COG_ERROR_NULL_PTR`, `COG_ERROR_NOT_FOUND`, `COG_ERROR_INVALID_ARG` |
| `cog_model_has_capability` | bool |
| `cog_endpoint_table_select_by_model` | `COG_OK`, `COG_ERROR_NULL_PTR`, `COG_ERROR_NOT_FOUND`, `COG_ERROR_INVALID_ARG` |

## Validation semantics

- `cog_provider_request_validate` rejects a NULL request (`NULL_PTR`), a
  NULL `model_id`, a NULL `messages` with non-zero `message_count`, and a
  NULL `tools_json` with non-zero `tools_json_len` (all `INVALID_ARG`).
  Zero-init defaults validate as OK once `model_id` is set.
- `cog_provider_response_validate` rejects a NULL response (`NULL_PTR`)
  and a NULL `content` with non-zero `content_capacity` (`INVALID_ARG`).
  A zero-capacity response (content may be NULL) is valid.

## Static vtable dispatch

- `cog_provider_connect` dispatches through `endpoint->vtable->connect` and
  returns `COG_ERROR_NULL_PTR` if `endpoint`, `endpoint->vtable`, or
  `out_provider` is NULL.
- `cog_provider_disconnect` / `cog_provider_send` / `cog_provider_receive`
  dispatch through the vtable embedded in the provider instance.  The
  opaque `cog_provider_t` layout is defined in the private
  `src/provider_internal.h`: its first member is a pointer to the static
  vtable that created it.  Provider implementations must embed that
  pointer first so the wrappers can reach the vtable.  This is a private
  implementation detail; consumers only see the opaque type.
- `cog_provider_disconnect(NULL)` is a documented no-op returning `COG_OK`.
- `cog_provider_send` / `cog_provider_receive` return `COG_ERROR_NULL_PTR`
  when either argument is NULL.

## Endpoint / model registry

- `cog_endpoint_table_lookup` finds the first endpoint whose `name` equals
  the query.  `out_endpoint` may be NULL for a presence-only check.
- `cog_endpoint_model_lookup` finds the first model whose `id` equals the
  query within an endpoint's model table.  Returns `COG_ERROR_NULL_PTR` if
  `endpoint`, `endpoint->models`, or `id` is NULL.
- `cog_model_has_capability` returns false for a NULL model and tests a
  single capability bit otherwise.
- `cog_endpoint_table_select_by_model` scans every endpoint and its model
  table, returning the first endpoint/model pair that serves the model id.
  `out_endpoint` and `out_model` may each be NULL.
- **Malformed tables fail closed.**  A non-zero `count` with a NULL backing
  array (`table->endpoints` / `models->models`) returns
  `COG_ERROR_INVALID_ARG` rather than dereferencing NULL.  This is a
  defensive extension beyond the header's documented error set and is
  pinned by the tests.

## Streaming / completion

- Both single-turn and streaming calls use the same `send`/`receive`
  operations.  A streaming request sets `request.stream`; the caller loops
  on `cog_provider_receive` until `response.is_complete` is true.
- `response.content` is a caller-provided buffer with an explicit
  `content_capacity`.  A chunk that does not fit returns
  `COG_ERROR_OVERFLOW`; the provider must not write past the buffer.
- `response.finish_reason` distinguishes natural stop, token limit, tool
  calls, and provider-reported errors.

## Errors and retry metadata

- `cog_provider_error_t` carries a result code, an HTTP status, a message,
  a `retryable` flag, and a suggested `retry_after_ms` (e.g. from a
  `Retry-After` header).  Scenario 12 (rate limit → backoff) and scenario
  16 (error propagation) rely on this detail.
- The abstraction layer does not implement a provider-level rate-limit
  queue or a second HTTP stack.  It uses the existing cog-net transport
  (see `libraries/cog-net`) for real HTTP paths; the test drives that path
  through a net-backed fake vtable against a local loopback server.

## Cancellation

- The vtable has no cancel slot; cancellation is provider-specific.  A
  provider signals a cancelled stream by returning `COG_ERROR_ABORTED`
  from `cog_provider_receive`.  The net-backed fake provider demonstrates
  this by cancelling the underlying cog-net request and surfacing
  `COG_ERROR_ABORTED`.

## Threading

Single-threaded per provider instance.  Endpoint and model tables are
read-only after construction and may be shared across threads.  A single
`cog_provider_t` must be driven by one thread at a time.

## Ownership

- `cog_provider_t` is allocated by the provider's connect function and
  released by its disconnect function; the caller owns it between those
  calls.
- Request and response buffers are caller-allocated; the provider never
  allocates hidden buffers and must not retain request pointers after the
  call.
- Endpoint and model tables are caller-owned static const data and must
  outlive any code that references them.
- There is no global mutable state in the library.

## Invariants

- `cog_provider_connect` sets `*out_provider` to NULL on failure and to a
  valid provider on success.
- `cog_provider_disconnect` releases all provider storage; the provider
  must not be used afterwards.
- A provider that has not sent a request returns `COG_ERROR_INVALID_STATE`
  from receive; a second send on an in-flight provider returns
  `COG_ERROR_INVALID_STATE`.
- Endpoint and model lookups leave `out_*` pointers unchanged on failure.

## Verification

`tests/unit/test_provider.c` drives the API with fake static vtables and a
local loopback HTTP server (fork-per-connection, no external network) and
pins:

- request/response validation (NULL, malformed, boundary)
- endpoint/model lookup, capability, and model selection (including
  presence-only and not-found paths)
- static vtable dispatch and NULL-argument semantics
- message history passed through to the provider
- single-turn and streaming responses, including overflow
- detailed provider errors with `retryable` / `retry_after_ms`
- cancellation (`COG_ERROR_ABORTED`)
- a real transport path through cog-net: streaming completion, 429
  `Retry-After` metadata, malformed responses, overflow, and cancellation
- ownership and NULL/not-found behavior

Run with `ctest -R unit-provider` (plain, ASan/UBSan, and Clang builds all

### Core integration (ticket 1433731)

`tests/integration/test_core_provider.c` wires a real cog-core agent to a
fake static-vtable provider and drives the full user-input -> send ->
LLM_CHUNK -> LLM_COMPLETE flow through the real dispatch table and session
history.  It pins:

- streaming: each chunk is appended to the session tree as an assistant
  message and the final chunk closes the turn with LLM_COMPLETE;
- error/retry metadata: a receive failure carrying a `cog_provider_error_t`
  (http_status, retryable, retry_after_ms, message) is read off the
  provider by the handler and propagated out of `cog_core_run`;
- cancellation: a receive returning `COG_ERROR_ABORTED` is handled
  gracefully (the turn returns to IDLE without appending a partial
  response);
- provider accessor/ownership: `cog_core_provider` returns the configured
  instance and the core borrows it (`cog_core_free` does not disconnect
  it).

The fake provider embeds the same first-member vtable layout as the
private `struct cog_provider` so the public `cog_provider_*` wrappers
dispatch correctly, but the test never includes the private header.

Run with `ctest -R integration-core-provider`.
pass).