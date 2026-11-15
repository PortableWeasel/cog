# core.h Contract

`libraries/cog-core/include/cog/core.h` — the agent core: construction and
destruction, the run loop, stop, enqueue, the state/running inspectors, the
provider/obs/store/ipc accessors, and the session-scoped pending-message
FIFO. Implemented by `libraries/cog-core/src/core.c`.

This document records the executable contract verified by
`tests/unit/test_core.c` and the cross-module effects verified by
`tests/integration/test_core_session.c`. It intentionally does **not**
expand the public API; it only documents what the header already declares.

Since the reviewed merge (64e536b), `core.c` also hosts the subagent
registry: `struct cog_core` embeds a `cog_agent_registry_t` (see
`src/agent_internal.h`), `cog_core_agent_registry()` exposes it to
`agent.c`, and `cog_core_free` releases it via
`cog_agent_registry_reset()`.  The registry is zero-initialised by
`cog_core_new` (calloc) and adds no new failure path to construction.

## Status

Implemented. `src/core.c` provides every function declared in `core.h`.
Guard: `COG_CORE_H`. The header includes the other cog-core public headers
(`agent.h`, `event.h`, `handler.h`, `ipc.h`, `obs.h`, `result.h`,
`session.h`, `session_store.h`, `state.h`, `tool.h`) and forward-declares
`cog_provider_t`.

## Public types

| Type | Kind | Notes |
|------|------|-------|
| `cog_core_t` | `struct cog_core` | Opaque agent core; created with `cog_core_new`, destroyed with `cog_core_free`. |
| `cog_wait_fn` | `void (*)(void *)` | Hook called by `cog_core_run` when the event queue is empty. |
| `cog_core_config_t` | `struct cog_core_config` | Caller-supplied configuration; zero-init for defaults. |

### `cog_core_config_t` fields and ownership

| Field | Required | Ownership | Notes |
|-------|----------|-----------|-------|
| `session` | yes | borrowed | Must be non-NULL and outlive the core. |
| `dispatch_table` | yes | borrowed | Must be non-NULL, valid for the core's lifetime. |
| `dispatch_table_size` | yes | — | Must be greater than zero. |
| `tool_table` | no | borrowed | May be NULL. |
| `event_queue_capacity` | no | — | Zero selects a sensible default (64). |
| `wait_fn` / `wait_arg` | no | borrowed | May be NULL (busy-wait with a short yield). |
| `ipc_endpoint` | no | owned | May be NULL (single-process). If non-NULL, the core takes ownership: it drains the endpoint during `cog_core_run` (accepting peers and receiving messages as `COG_EVENT_IPC_*` events) and destroys it in `cog_core_free`. The endpoint must be bound (listening or connected); an unbound endpoint is accepted but never drained. The caller must not use or free the endpoint after `cog_core_new`. |
| `local_peer_id` | no | — | Zero = unidentified. |
| `provider` | no | borrowed | Optional; may be NULL for tests / no-provider operation. Stored as-is. |
| `obs` | no | borrowed | May be NULL. |
| `store` | no | borrowed | May be NULL. When non-NULL and the store's `auto_save` flag is true, the core auto-saves the session once after each successfully handled `COG_EVENT_LLM_COMPLETE` exchange (see `cog_core_run`). |

## Functions

### `cog_core_new`

```c
cog_result_t cog_core_new(cog_core_t **out, const cog_core_config_t *config);
```

| Return | Condition |
|--------|-----------|
| `COG_OK` | A valid core was created; `*out` is set and owned by the caller. |
| `COG_ERROR_NULL_PTR` | `out` or `config` is NULL. |
| `COG_ERROR_INVALID_ARG` | `config->session` is NULL, `config->dispatch_table` is NULL, or `config->dispatch_table_size` is zero. |
| `COG_ERROR_NOMEM` | Allocation of the core, its event queue, its pending FIFO, or (when an IPC endpoint is configured) its receive buffer fails. |

- On failure `*out` is NULL and no resources are held.
- The core starts in state `0` (the first application state; the header
  provides no initial-state field).
- The core marks the session's private `attached` flag so
  `cog_session_reset` returns `COG_ERROR_INVALID_STATE` while the core is
  alive (see session.c).
- `provider`, `obs`, and `store` are stored as-is (they may be NULL); the
  provider is documented optional and may be NULL for tests or
  no-provider operation.
- If `config->ipc_endpoint` is non-NULL, the core takes ownership of it,
  allocates a receive buffer sized to accept every valid frame
  (`UINT16_MAX` bytes), and probes the endpoint's role (listener,
  connected, or unbound).  On success the core logs `core.created`
  through `obs` (when configured).

### `cog_core_free`

```c
void cog_core_free(cog_core_t *core);
```

- NULL `core` is a no-op.
- Releases the event queue, the pending FIFO (and every queued message),
  the subagent registry, and the core struct.
- Clears the session's `attached` flag so the session can be reset/freed
  independently.  The session itself is not freed (caller-owned).
- Releases every in-flight IPC message payload copy that was never
  dispatched, frees every accepted client (each disconnect removes it
  from the listener's peer table), destroys the core's own IPC endpoint
  (unlinking a listener's socket path), and releases the receive buffer.
- Logs `core.destroyed` through `obs` (when configured).

### `cog_core_run`

```c
cog_result_t cog_core_run(cog_core_t *core);
```

| Return | Condition |
|--------|-----------|
| `COG_OK` | The loop exited because `cog_core_stop` was called. |
| `COG_ERROR_NULL_PTR` | `core` is NULL. |
| `COG_ERROR_INVALID_STATE` | The core is already running. |
| `COG_ERROR_NOT_FOUND` | No dispatch row matches the current `(state, event->id)`. |
| `COG_ERROR_UNHANDLED` | The matching row has a NULL handler. |
| handler status | A handler returned a non-`COG_OK` status. |
| `COG_ERROR_TRANSITION_DENIED` | A handler requested a transition not in the row's allowed list. |
| save error | Auto-save of the session failed after a successful `COG_EVENT_LLM_COMPLETE` exchange. |

- Loop: pop one event, look up `(state, event->id)` in the static table,
  invoke the handler, validate the requested transition against the row's
  allowed list, and apply it (`COG_TRANSITION_SELF` keeps the state).
- On any dispatch failure the state is left unchanged and the loop returns
  the error.
- **Auto-save:** after a `COG_EVENT_LLM_COMPLETE` handler returns `COG_OK`
  and its transition is allowed and applied, if `core->store != NULL` and
  `cog_store_auto_save(core->store)` is true, the core calls
  `cog_store_save(core->store, core->session)` exactly once.  The save
  error (if any) is returned from the loop; the state transition has
  already been applied.  No save occurs on handler failure, denied
  transition, cancellation (`COG_EVENT_CANCEL` / `COG_EVENT_CANCELLED`),
  shutdown (`COG_EVENT_SHUTDOWN`), streaming chunks
  (`COG_EVENT_LLM_CHUNK`), or any other event.
- When the queue is empty the loop first honours a pending stop request
  (exits without dequeuing a pending message), then drains the IPC fabric
  (see below), then tries to inject the oldest pending message for the
  core's session as a `COG_EVENT_USER_INPUT`; if none is pending it calls
  `wait_fn(wait_arg)`, or busy-waits with a short yield when `wait_fn` is
  NULL.
- **IPC drain.**  When an endpoint is configured, each empty-queue pass
  drains it with non-blocking polls (timeout 0): a listener accepts every
  pending connection (enqueuing `COG_EVENT_IPC_CONNECTED` per peer and
  tracking the client), and every connected endpoint (the core's own or
  an accepted client) is polled and received from.  A received frame is
  enqueued as `COG_EVENT_IPC_MESSAGE` whose `cog_event_ipc_t` carries the
  peer id and a core-owned copy of the payload; the copy is released
  after the event is dispatched.  A disconnect or malformed frame (recv
  `COG_ERROR_IO` / `COG_ERROR_OVERFLOW`) enqueues
  `COG_EVENT_IPC_DISCONNECTED` and frees the client (accepted peers) or
  leaves the core's own endpoint unbound.  A full event queue stops the
  drain (remaining frames stay in the socket buffer); an allocation
  failure aborts the loop and is returned.  Because the IPC transport is
  blocking, a peer that stalls mid-frame can block the drain until the
  frame completes or the connection closes; the core never blocks on
  poll, so an idle fabric never stalls the loop.
- The loop logs `core.run_start` on entry and, on exit, `core.run_stop`
  (clean stop) or `core.run_error` (non-`COG_OK` result) through `obs`
  (when configured).
- `cog_core_stop` may be called from another thread or a signal handler;
  the loop exits at the next safe opportunity.
- After the loop the running flag is cleared.

### `cog_core_stop`

```c
void cog_core_stop(cog_core_t *core);
```

- NULL `core` is a no-op.
- Sets the stop request and clears the running flag.  The loop exits after
  the current wait/dispatch cycle.

### `cog_core_enqueue`

```c
cog_result_t cog_core_enqueue(cog_core_t *core, const cog_event_t *event);
```

| Return | Condition |
|--------|-----------|
| `COG_OK` | The event was copied into the core's queue. |
| `COG_ERROR_NULL_PTR` | `core` or `event` is NULL. |
| `COG_ERROR_OVERFLOW` | The internal queue is full. |

### Inspectors and accessors

| Function | NULL core | Non-NULL core |
|----------|-----------|---------------|
| `cog_core_session` | NULL | the configured session |
| `cog_core_state` | `0` | the current state id |
| `cog_core_is_running` | `false` | the running flag |
| `cog_core_ipc` | NULL | the configured endpoint (owned by the core; destroyed by `cog_core_free`) |
| `cog_core_provider` | NULL | the provider (or NULL if not configured) |
| `cog_core_obs` | NULL | the obs context (or NULL if not configured) |
| `cog_core_store` | NULL | the store (or NULL if not configured) |

### Pending message FIFO

```c
cog_result_t cog_core_queue_message(cog_core_t *core, const char *session_id,
                                    const char *text, size_t text_len);
cog_result_t cog_core_dequeue_message(cog_core_t *core, const char *session_id,
                                      char *out_buf, size_t buf_capacity,
                                      size_t *out_len);
cog_result_t cog_core_pending_count(const cog_core_t *core,
                                    const char *session_id, size_t *out_count);
```

- The FIFO is a single bounded array (default capacity 8) whose entries are
  tagged with a session id.  `queue_message` appends; `dequeue_message` and
  `pending_count` filter by session id, giving per-session isolation while
  keeping the whole FIFO bounded.
- `queue_message` copies `text` (and the session id).  Returns
  `COG_ERROR_NULL_PTR` if `core`, `session_id`, or `text` is NULL;
  `COG_ERROR_OVERFLOW` when the FIFO is full; `COG_ERROR_NOMEM` on
  allocation failure.  A `text_len` of `SIZE_MAX` is rejected with
  `COG_ERROR_NOMEM` because `text_len + 1` would wrap the allocation size
  to zero and then write past it.
- `dequeue_message` removes and returns the oldest message for the session.
  Returns `COG_ERROR_NULL_PTR` if `core`, `session_id`, or `out_buf` is
  NULL; `COG_ERROR_NOT_FOUND` when the session has no pending message;
  `COG_ERROR_OVERFLOW` when the message does not fit in `buf_capacity`
  (on this error `*out_len` receives the required capacity excluding the
  terminator, and the message is left queued).  On success the buffer is
  NUL-terminated and `*out_len` (if non-NULL) holds the length excluding
  the terminator.
- `pending_count` returns the number of pending messages for the session.
  Returns `COG_ERROR_NULL_PTR` if `core`, `session_id`, or `out_count` is
  NULL.
- **Auto-injection:** when the run loop finds the event queue empty, it
  dequeues the oldest pending message for the core's own session and
  enqueues it as a `COG_EVENT_USER_INPUT` whose payload borrows the message
  text.  Because injection happens only when the queue has drained, pending
  messages are delivered after the current turn's events have been
  processed.  The text is owned by the core until the injected event is
  dispatched, then released.

## Error semantics

All functions return `COG_OK` on success and a negative code on failure,
per `cog/result.h`. Codes produced by this module: `COG_OK`,
`COG_ERROR_INVALID_ARG`, `COG_ERROR_NULL_PTR`, `COG_ERROR_NOMEM`,
`COG_ERROR_NOT_FOUND`, `COG_ERROR_OVERFLOW`, `COG_ERROR_UNHANDLED`,
`COG_ERROR_TRANSITION_DENIED`, `COG_ERROR_INVALID_STATE`, and any status
returned by a handler.

## Invariants

- The core owns its event queue, its pending FIFO, and the subagent
  registry; all are released by `cog_core_free`.
- The session, provider, obs, and store are borrowed; the core never frees
  them.
- The core starts in state `0` and only changes state through a validated
  handler transition.
- A failed dispatch never changes the state.
- The pending FIFO is bounded (8 entries) and per-session isolated.
- The module holds no global mutable state.

## Threading

Per header: a single `cog_core_t` is driven by one thread calling
`cog_core_run`. `cog_core_stop` may be called from another thread or a
signal handler. `cog_core_enqueue` is single-producer with respect to the
queue. The pending-message functions are not thread-safe.

The stop request and the running flag are C11 `_Atomic bool` fields. The
run loop reads the stop request with acquire ordering and `cog_core_stop`
writes it with release ordering, so a concurrent stop is guaranteed to be
observed before the loop dequeues or injects any pending message (the
stop-before-inject guarantee holds under concurrency). The running flag is
a relaxed status snapshot; `cog_core_is_running` may be called from any
thread but the result is not a synchronisation point.

## Ownership

- **Borrowed:** session, provider, obs, store, dispatch table, tool table,
  wait hook.
- **Owned:** event queue, pending FIFO (and each queued message), the
  injected-message payload while in flight, the subagent registry (agent
  records, their owned strings, and their child sessions), and (when
  configured) the IPC endpoint, its accepted clients, the receive buffer,
  and every in-flight IPC payload copy.

## Session attachment

`cog_core_new` sets the session's private `attached` flag and
`cog_core_free` clears it. The flag lives in the single private layout
`libraries/cog-core/src/session_internal.h`, shared by session.c and
core.c (the public type is opaque and there is no public attach API).
While attached, `cog_session_reset` returns `COG_ERROR_INVALID_STATE`.
This is the cross-module effect verified by
`tests/integration/test_core_session.c`.

## Note on provider NULL

The provider is documented optional (may be NULL for tests or no-provider
operation). The implementation stores a NULL provider as-is and the
accessor returns NULL; no error is raised.

## Verification

`tests/unit/test_core.c` verifies: construction validation (NULL out/config,
NULL session, NULL dispatch table, zero table size, NOMEM via an oversized
queue capacity), NULL-free no-op, the inspectors/accessors for NULL and
non-NULL cores, enqueue NULL/overflow, the run loop happy path (dispatch,
handler invocation, transition application, wait-hook driving, clean stop),
the already-running guard via a re-entrant `cog_core_run`, empty-queue stop,
dispatch errors (NOT_FOUND, handler status, TRANSITION_DENIED), IPC
endpoint acceptance (a real endpoint is accepted, exposed by
`cog_core_ipc`, and destroyed by `cog_core_free`), and the pending FIFO
(NULL checks, FIFO order, round-trip, buffer sizing overflow,
bounded-capacity overflow, not-found, per-session isolation, and automatic
injection into session history).

`tests/integration/test_core_ipc.c` verifies the core+IPC integration over
real local Unix domain sockets: endpoint acceptance and ownership, a
listener core accepting a peer (`COG_EVENT_IPC_CONNECTED`), an incoming
message dispatched as `COG_EVENT_IPC_MESSAGE` with peer/session metadata,
disconnect handling (`COG_EVENT_IPC_DISCONNECTED`), a malformed oversized
frame closing the connection without deadlocking the loop, the core as a
connected endpoint, and destruction (the listener's socket path is
unlinked by `cog_core_free`).

`tests/integration/test_core_obs.c` verifies the core+obs integration: a
core configured with a real obs context logs `core.created`,
`core.run_start`, `core.run_stop`/`core.run_error`, and `core.destroyed`
through a capture sink, with no hidden allocations (clean under
ASan/UBSan/LSan).

`tests/integration/test_core_provider.c` (ticket 1433731) verifies the
core+provider integration: a real core configured with a fake
static-vtable provider drives a user-input -> send -> LLM_CHUNK ->
LLM_COMPLETE flow through real dispatch and session history, covering
streaming content, error/retry metadata propagation, cancellation, and
provider accessor/ownership (the core borrows the provider and does not
disconnect it on `cog_core_free`).

`tests/integration/test_core_toolkit.c` (ticket 1433731) verifies the
core+toolkit integration: a real core/static tool table dispatches to the
toolkit, executes independent tools concurrently (proving overlap),
collects exactly `call_count` ordered results into session history, and
covers timeout, cooperative cancellation, and tool error propagation
without data races (clean under TSan).

`tests/integration/test_core_session.c` verifies the cross-module effects:
session attachment (reset refused while attached, allowed after teardown),
run-loop mutation of session history, and pending-message auto-injection
appearing in session history in FIFO order.

`tests/integration/test_store_session.c` verifies the auto-save trigger:
with `auto_save=false` a full exchange leaves the store empty; with
`auto_save=true` exactly one session is saved after one `LLM_COMPLETE`
turn and is loadable as a real tree; a failing backend propagates its
error from `cog_core_run` while leaving the applied state and session
intact; `LLM_CHUNK` events never save (only `LLM_COMPLETE` does); and the
borrowed store outlives `cog_core_free` and remains usable.

All tests are compiled and run with strict warning flags and ASan/UBSan
through the CMake/CTest build.

## Build / export (ticket 1433731)

`cog-core` and `cog-result` (and, for the core+provider/core+toolkit
integration, `cog-provider`, `cog-toolkit`, `cog-ipc`, and `cog-obs`) set
`EXPORT_NAME` so the installed export sets produce the same target names
as the build-tree aliases: `cog::core`, `cog::result`, `cog::provider`,
`cog::toolkit`, `cog::ipc`, `cog::obs`.  An external consumer can
`include()` the installed `*-targets.cmake` files in dependency order
(result -> ipc -> obs -> core -> provider/toolkit) and link `cog::provider`
/ `cog::toolkit` with correct include and link dependencies and no cycles.
This is verified by an external consumer build against a staged install.