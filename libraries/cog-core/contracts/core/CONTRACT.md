# core.h Contract

`libraries/cog-core/include/cog/core.h` — the agent core: construction and
destruction, the run loop, stop, enqueue, the state/running inspectors, the
provider/obs/store/ipc accessors, and the session-scoped pending-message
FIFO. Implemented by `libraries/cog-core/src/core.c`.

This document records the executable contract verified by
`tests/unit/test_core.c` and the cross-module effects verified by
`tests/integration/test_core_session.c`. It intentionally does **not**
expand the public API; it only documents what the header already declares.

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
| `ipc_endpoint` | no | owned | If non-NULL the core takes ownership; release is deferred to the IPC phase (see Ownership). |
| `local_peer_id` | no | — | Zero = unidentified. |
| `provider` | no | borrowed | Stored as-is; may be NULL. |
| `obs` | no | borrowed | May be NULL. |
| `store` | no | borrowed | May be NULL. |

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
| `COG_ERROR_NOMEM` | Allocation of the core, its event queue, or its pending FIFO fails. |

- On failure `*out` is NULL and no resources are held.
- The core starts in state `0` (the first application state; the header
  provides no initial-state field).
- The core marks the session's private `attached` flag so
  `cog_session_reset` returns `COG_ERROR_INVALID_STATE` while the core is
  alive (see session.c).
- `provider`, `obs`, and `store` are stored as-is (they may be NULL); the
  header's `@return` list does not reject a NULL provider, so the
  implementation does not either.  This is a deliberate reading of the
  documented error contract; see the note at the end of this document.

### `cog_core_free`

```c
void cog_core_free(cog_core_t *core);
```

- NULL `core` is a no-op.
- Releases the event queue, the pending FIFO (and every queued message),
  and the core struct.
- Clears the session's `attached` flag so the session can be reset/freed
  independently.  The session itself is not freed (caller-owned).
- The IPC endpoint is not released in this phase: its destructor
  (`cog_ipc_endpoint_free`) belongs to the not-yet-implemented cog-ipc
  library.  See Ownership.

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

- Loop: pop one event, look up `(state, event->id)` in the static table,
  invoke the handler, validate the requested transition against the row's
  allowed list, and apply it (`COG_TRANSITION_SELF` keeps the state).
- On any dispatch failure the state is left unchanged and the loop returns
  the error.
- When the queue is empty the loop first tries to inject the oldest pending
  message for the core's session as a `COG_EVENT_USER_INPUT`; if none is
  pending it calls `wait_fn(wait_arg)`, or busy-waits with a short yield
  when `wait_fn` is NULL.
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
| `cog_core_ipc` | NULL | the IPC endpoint (or NULL if single-process) |
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
  allocation failure.
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

- The core owns its event queue and its pending FIFO; both are released by
  `cog_core_free`.
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

## Ownership

- **Borrowed:** session, provider, obs, store, dispatch table, tool table,
  wait hook.
- **Owned:** event queue, pending FIFO (and each queued message), the
  injected-message payload while in flight.
- **IPC endpoint:** the header says the core takes ownership, but
  `cog_ipc_endpoint_free` belongs to the not-yet-implemented cog-ipc
  library.  In this phase the core stores the endpoint pointer for the
  accessor and does not release it; releasing it is deferred to the IPC
  phase.  This is a documented, deliberate deviation forced by the
  dependency order, not a leak in the final design.

## Session attachment

`cog_core_new` sets the session's private `attached` flag (mirroring
session.c's struct layout, since the public type is opaque and there is no
public attach API). While attached, `cog_session_reset` returns
`COG_ERROR_INVALID_STATE`. `cog_core_free` clears the flag. This is the
cross-module effect verified by `tests/integration/test_core_session.c`.

## Note on provider NULL

The header's `cog_core_config_t::provider` documentation says the provider
"Must be non-NULL", but the `cog_core_new` `@return` list does not include a
provider-NULL error. The implementation follows the `@return` list exactly
and stores a NULL provider as-is (the accessor then returns NULL). This
keeps the documented error contract literal; if the Critic prefers rejecting
NULL provider as `COG_ERROR_INVALID_ARG`, that is a one-line change.

## Verification

`tests/unit/test_core.c` verifies: construction validation (NULL out/config,
NULL session, NULL dispatch table, zero table size, NOMEM via an oversized
queue capacity), NULL-free no-op, the inspectors/accessors for NULL and
non-NULL cores, enqueue NULL/overflow, the run loop happy path (dispatch,
handler invocation, transition application, wait-hook driving, clean stop),
the already-running guard via a re-entrant `cog_core_run`, empty-queue stop,
dispatch errors (NOT_FOUND, handler status, TRANSITION_DENIED), and the
pending FIFO (NULL checks, FIFO order, round-trip, buffer sizing overflow,
bounded-capacity overflow, not-found, per-session isolation, and automatic
injection into session history).

`tests/integration/test_core_session.c` verifies the cross-module effects:
session attachment (reset refused while attached, allowed after teardown),
run-loop mutation of session history, and pending-message auto-injection
appearing in session history in FIFO order.

Both tests are compiled and run with strict warning flags and ASan/UBSan
through the CMake/CTest build.