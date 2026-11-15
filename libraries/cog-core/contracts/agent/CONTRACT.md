> Stage 14.Q supersedes the historical allocation/capacity text below:
> core construction reserves max_agents records and max_agent_message_bytes
> string buffers (defaults 16 and 65536). Local copied task/status/result
> replacement cannot grow; exhaustion is atomic LIMIT. The new remote attempt,
> notification acknowledgment and release/reuse protocol remains N/O work.
> See the current core runtime contract and Q evidence for tested scope.

# agent.h Contract

`libraries/cog-core/include/cog/agent.h` — the subagent lifecycle: spawn,
cancel, status, notify_parent, result, session_id, and is_cancelled.
Implemented by `libraries/cog-core/src/agent.c`.

This document records the executable contract verified by
`tests/unit/test_agent.c`. It intentionally does **not** expand the public
API; it only documents what the header already declares, plus the private
orchestrator hooks in `src/agent_internal.h` that the white-box test and
the future orchestrator layer use.

## Status

Implemented. `src/agent.c` provides every function declared in `agent.h`.
Guard: `COG_AGENT_H`. The header includes `cog/result.h` and
`cog/session.h`, and forward-declares `cog_core_t`.

## Public types

| Type | Kind | Notes |
|------|------|-------|
| `cog_agent_id_t` | `uint64_t` | Unique subagent id; assigned by `cog_agent_spawn`. Zero is invalid. |
| `cog_agent_status_t` | enum | `PENDING`, `RUNNING`, `COMPLETED`, `FAILED`, `CANCELLED`, `TIMEOUT`. |
| `cog_agent_config_t` | struct | `task`, `parent_session_id`, `timeout_ms`, `force_local`, `group_id`. |

### `cog_agent_config_t` fields

| Field | Required | Notes |
|-------|----------|-------|
| `task` | yes | Must be non-NULL and non-empty. |
| `parent_session_id` | no | May be NULL (no parent link). Copied. |
| `timeout_ms` | no | Zero = no limit. Recorded for the orchestrator. |
| `force_local` | no | Placement hint; recorded for the orchestrator. |
| `group_id` | no | Zero = no group. Recorded for the orchestrator. |

## Functions

### `cog_agent_spawn`

```c
cog_result_t cog_agent_spawn(cog_core_t *core, const cog_agent_config_t *config,
                             cog_agent_id_t *out_id);
```

| Return | Condition |
|--------|-----------|
| `COG_OK` | A child session was created, linked to the parent, and the agent recorded as `PENDING`; `*out_id` is set. |
| `COG_ERROR_NULL_PTR` | `core`, `config`, or `out_id` is NULL. |
| `COG_ERROR_INVALID_ARG` | `config->task` is NULL or empty. |
| `COG_ERROR_NOMEM` | Allocation of the registry array, the session-id copy, or the parent-id copy fails. |
| `COG_ERROR_UNAVAILABLE` | The core's registry is full (`COG_AGENT_MAX_PER_CORE` = 256 live agents). |

- On success the agent id is non-zero and unique within the core; ids are
  assigned monotonically from 1 per core.
- The child session is created with `cog_session_new` (generated id) and
  linked with `cog_session_set_parent(config->parent_session_id)`. The
  agent record owns the child session.
- The registry is append-only for the lifetime of the core: agent.h
  declares no destroy API, so agents are released wholesale by
  `cog_core_free`.
- `force_local` is a placement hint recorded on the agent record (like
  `group_id` and `timeout_ms`) for the orchestrator to honour; in this
  phase every spawn is local.

### `cog_agent_cancel`

```c
cog_result_t cog_agent_cancel(cog_core_t *core, cog_agent_id_t agent_id);
```

| Return | Condition |
|--------|-----------|
| `COG_OK` | The agent was not terminal; it is now `CANCELLED` and `is_cancelled` is true. |
| `COG_ERROR_NULL_PTR` | `core` is NULL. |
| `COG_ERROR_NOT_FOUND` | `agent_id` does not exist. |
| `COG_ERROR_INVALID_STATE` | The agent is already in a terminal state (`COMPLETED`, `FAILED`, `CANCELLED`, `TIMEOUT`). |

- Cancellation is immediate (local semantics): the status becomes
  `CANCELLED` and the cancelled flag is set in the same call.  Remote
  cancellation (async, forwarded via IPC) is the orchestrator's job.
- A second cancel on the same agent returns `COG_ERROR_INVALID_STATE`; the
  cancelled flag stays set (idempotent observable state).

### `cog_agent_status`

```c
cog_result_t cog_agent_status(cog_core_t *core, cog_agent_id_t agent_id,
                              cog_agent_status_t *out_status);
```

| Return | Condition |
|--------|-----------|
| `COG_OK` | `*out_status` receives the current status. |
| `COG_ERROR_NULL_PTR` | `core` or `out_status` is NULL. |
| `COG_ERROR_NOT_FOUND` | `agent_id` does not exist. |

### `cog_agent_notify_parent`

```c
cog_result_t cog_agent_notify_parent(cog_core_t *core, cog_agent_id_t agent_id,
                                     const char *status_msg);
```

| Return | Condition |
|--------|-----------|
| `COG_OK` | The message was copied onto the agent record (replacing any previous message). |
| `COG_ERROR_NULL_PTR` | `core` or `status_msg` is NULL. |
| `COG_ERROR_NOT_FOUND` | `agent_id` does not exist. |
| `COG_ERROR_NOMEM` | Allocation of the copy fails. |

- **Scope note:** the header documents that the parent receives a
  `COG_EVENT_SUBAGENT_STATUS` event.  In this phase (no orchestrator, no
  IPC) the function records the message on the agent record; the
  orchestrator layer (Stage 12.5) reads it via `cog_agent_internal_get`
  and delivers the event to the parent session.  This deliberately avoids
  enqueueing an event whose payload lifetime the core cannot track.

### `cog_agent_result`

```c
cog_result_t cog_agent_result(cog_core_t *core, cog_agent_id_t agent_id,
                              char *out_result, size_t result_capacity,
                              size_t *out_result_len);
```

| Return | Condition |
|--------|-----------|
| `COG_OK` | The result was copied, NUL-terminated, and `*out_result_len` (if non-NULL) set to the length excluding the terminator. |
| `COG_ERROR_NULL_PTR` | `core` or `out_result` is NULL. |
| `COG_ERROR_NOT_FOUND` | `agent_id` does not exist. |
| `COG_ERROR_INVALID_STATE` | The agent has not completed (any status other than `COMPLETED`). |
| `COG_ERROR_OVERFLOW` | The result does not fit in `result_capacity` (including the terminator); `*out_result_len` (if non-NULL) receives the required capacity excluding the terminator. |

- An empty result needs a capacity of at least 1 (for the terminator); a
  zero-capacity buffer always overflows.

### `cog_agent_session_id`

```c
cog_result_t cog_agent_session_id(cog_core_t *core, cog_agent_id_t agent_id,
                                  const char **out_session_id);
```

| Return | Condition |
|--------|-----------|
| `COG_OK` | `*out_session_id` points to the agent's child session id (owned by the registry; valid until the core is freed). |
| `COG_ERROR_NULL_PTR` | `core` or `out_session_id` is NULL. |
| `COG_ERROR_NOT_FOUND` | `agent_id` does not exist. |

### `cog_agent_is_cancelled`

```c
cog_result_t cog_agent_is_cancelled(cog_core_t *core, cog_agent_id_t agent_id,
                                    bool *out_cancelled);
```

| Return | Condition |
|--------|-----------|
| `COG_OK` | `*out_cancelled` receives the cancelled flag. |
| `COG_ERROR_NULL_PTR` | `core` or `out_cancelled` is NULL. |
| `COG_ERROR_NOT_FOUND` | `agent_id` does not exist. |

- The flag is set by `cog_agent_cancel` and never cleared.  Handlers use
  it to discard late results from cancelled agents (fan-out fan-in).

## Status state machine

```
PENDING  --start-->   RUNNING
PENDING  --cancel-->  CANCELLED
RUNNING  --complete--> COMPLETED
RUNNING  --fail-->    FAILED
RUNNING  --cancel-->  CANCELLED
RUNNING  --timeout--> TIMEOUT
```

Terminal states (`COMPLETED`, `FAILED`, `CANCELLED`, `TIMEOUT`) accept no
further transitions: every transition hook returns
`COG_ERROR_INVALID_STATE`.  In particular, a late result delivered after
cancellation is rejected, so the cancelled state is never overwritten.

## Private orchestrator hooks (`src/agent_internal.h`)

These are not public API; they are the hooks the orchestrator layer
(Stage 12.5) will call to drive execution, and the white-box unit test
uses them to exercise the state machine:

| Function | Transition | Errors |
|----------|-----------|--------|
| `cog_agent_internal_start` | `PENDING` → `RUNNING` | NULL_PTR, NOT_FOUND, INVALID_STATE |
| `cog_agent_internal_complete(core, id, result, result_len)` | `RUNNING` → `COMPLETED` | NULL_PTR, INVALID_ARG (NULL result with non-zero len), NOT_FOUND, INVALID_STATE, NOMEM (incl. `SIZE_MAX` length wrap guard) |
| `cog_agent_internal_fail` | `RUNNING` → `FAILED` | NULL_PTR, NOT_FOUND, INVALID_STATE |
| `cog_agent_internal_timeout` | `RUNNING` → `TIMEOUT` | NULL_PTR, NOT_FOUND, INVALID_STATE |
| `cog_agent_internal_get` | read-only record access | NULL on NULL core / unknown id |

`cog_agent_internal_get` returns a pointer into the registry's dynamic
array.  The pointer is **invalidated by any subsequent spawn that grows
the registry** (capacity boundaries 8 -> 16 -> ... -> 256, which
reallocates the array) and by `cog_core_free` (registry reset).  It is
safe to hold across in-place mutations (cancel, status, notify_parent,
result, session_id, is_cancelled, and the internal transition hooks),
which never reallocate.  Orchestrator consumers must re-fetch the
pointer after any spawn and never use it after `cog_core_free`.

The registry itself is embedded in `struct cog_core` (see
`src/agent_internal.h`); `core.c` exposes it via
`cog_core_agent_registry()` and releases it via
`cog_agent_registry_reset()` in `cog_core_free`.

## Error semantics

All functions return `COG_OK` on success and a negative code on failure,
per `cog/result.h`. Codes produced by this module: `COG_OK`,
`COG_ERROR_INVALID_ARG`, `COG_ERROR_NULL_PTR`, `COG_ERROR_NOMEM`,
`COG_ERROR_NOT_FOUND`, `COG_ERROR_OVERFLOW`, `COG_ERROR_INVALID_STATE`,
`COG_ERROR_UNAVAILABLE`.

## Invariants

- Agent ids are non-zero and unique per core; each core's counter starts
  at 1.
- The registry is append-only and bounded (`COG_AGENT_MAX_PER_CORE` =
  256); a full registry makes spawn return `COG_ERROR_UNAVAILABLE`.
- Every string in an agent record is an owned copy; the child session is
  owned by the record.  All are released by `cog_core_free`.
- The cancelled flag, once set, is never cleared; terminal statuses are
  never overwritten.
- The module holds no global mutable state (the registry lives in the
  core instance).

## Threading

Per header: agent functions are called from the thread running
`cog_core_run()` and are not thread-safe.  The registry is not
synchronised.

## Ownership

- **Borrowed:** `core`, `config`, `task`, `parent_session_id`,
  `status_msg`, `result` (copied, never retained).
- **Owned:** the registry array, each record's `session_id`,
  `parent_session_id`, `result`, `status_msg`, and the child session.
  All released by `cog_core_free` via `cog_agent_registry_reset`.
- **Outputs:** `*out_id`, `*out_status`, `*out_session_id`,
  `*out_result_len`, `*out_cancelled` are written only on the documented
  success paths (except `*out_result_len` on `COG_ERROR_OVERFLOW`, which
  receives the required capacity).

## Verification

`tests/unit/test_agent.c` verifies: spawn validation (NULL core/config/
out_id, NULL/empty task), the happy path (unique ids, PENDING status,
child session linked to the parent, group id, timeout, and force_local
recorded — including the false default for a zeroed config), the
registry-full boundary (`COG_ERROR_UNAVAILABLE` at 256 agents), status /
session_id / is_cancelled NULL and NOT_FOUND checks, cancellation
(PENDING and RUNNING, terminal refusal, idempotence, late-result
rejection), notify_parent (record and replace), result (round-trip,
exact length, NUL termination, overflow sizing, empty result,
zero-capacity overflow, INVALID_STATE before completion), every internal
transition (valid and invalid, terminal locking, argument validation),
per-core registry isolation, and cleanup under stress (200 agents in
mixed states, then `cog_core_free` — leak-checked by ASan/UBSan).

The test is compiled with strict warning flags and ASan/UBSan through the
CMake/CTest build.  It is a white-box test: it includes
`src/agent_internal.h` (the src directory is added to its include path)
to drive the private status-transition hooks.
