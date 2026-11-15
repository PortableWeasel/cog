# orchestrator.h Contract

`libraries/cog-orchestrator/include/cog/orchestrator.h` — the process
orchestrator: auxiliary-core spawn and process ownership, health
monitoring, placement, graceful shutdown with timeout, crash detection and
session recovery, and auto-replace. Implemented by
`libraries/cog-orchestrator/src/orchestrator.c`.

This document records the executable contract verified by
`tests/unit/test_orchestrator.c` and
`tests/integration/test_orchestrator_ipc.c`. It intentionally does **not**
expand the public API; it only documents what the header already declares
and the concrete behaviour the implementation pins down.

## Status

Implemented. `src/orchestrator.c` provides every function declared in
`orchestrator.h`. Guard: `COG_ORCHESTRATOR_H`. The header includes
`<cog/ipc.h>` and `<cog/result.h>` and the standard `<stdbool.h>` /
`<stddef.h>` / `<stdint.h>`.

## Architecture

The orchestrator lives only in the main core process. It owns its own IPC
listener socket (bound to `config->socket_path`). Auxiliary cores connect
back to it. The main core's run loop drives the orchestrator through the
private hooks in `src/orchestrator_internal.h`:

- `cog_orchestrator_poll` — reap dead children, accept pending
  connections, drain control messages, auto-replace dead cores.
- `cog_orchestrator_accept` — accept one pending connection.
- `cog_orchestrator_register_session` / `cog_orchestrator_unregister_session`
  — crash-recovery session tracking.
- `cog_orchestrator_set_now` / `cog_orchestrator_now` — test clock override.
- `cog_orchestrator_set_spawn_timeout` / `cog_orchestrator_set_ack_timeout`
  — test timeout overrides.

These hooks are private (not installed, not part of the public contract),
mirroring how cog-core exposes `agent_internal.h` to its white-box tests.

## Wire protocol

Control messages are newline-terminated text frames:

| Direction | Message | Meaning |
|-----------|---------|---------|
| aux → orch | `hello <pid>` | announce the OS pid (spontaneous connects) |
| aux → orch | `heartbeat <active>` | liveness + active subagent count |
| aux → orch | `session <id>` | register a session on this core |
| aux → orch | `ack` | acknowledge a shutdown command |
| orch → aux | `shutdown` | request graceful stop |

Trailing whitespace/newline is trimmed before parsing, so `ack\n` matches
`ack` and session ids are stored without the newline.

## Public types

| Type | Kind | Notes |
|------|------|-------|
| `cog_orchestrator_t` | `struct cog_orchestrator` | Opaque orchestrator. |
| `cog_health_t` | `enum` | `UNKNOWN` / `HEALTHY` / `DEGRADED` / `DEAD`. |
| `cog_core_info_t` | `struct` | Peer id, pid, health, load, capacity, last heartbeat. |
| `cog_orchestrator_config_t` | `struct` | Socket path, max aux cores, health interval, threshold, capacity, auto-replace. |

## Health model

Health is computed lazily from the last heartbeat timestamp and the
configured interval/threshold (no timer thread):

- `UNKNOWN` — no heartbeat received yet.
- `HEALTHY` — last heartbeat within `health_interval_ms`.
- `DEGRADED` — missed one interval (elapsed ≥ interval, < threshold×interval).
- `DEAD` — missed the threshold, or the child was reaped / socket closed.

A reaped or disconnected child is always `DEAD`. A fresh heartbeat restores
`HEALTHY`.

## Ownership and threading

- The caller owns the orchestrator (`cog_orchestrator_new` /
  `cog_orchestrator_free`). `free` SIGKILLs and reaps spawned children,
  closes every accepted endpoint, releases session tables, frees the
  listener, unlinks the socket path, and frees the context. `free(NULL)`
  is a no-op.
- Every child spawned by `cog_orchestrator_spawn` is reaped with
  `waitpid(WNOHANG)` inside `cog_orchestrator_poll` (zombie-free).
- Dead cores stay in the managed table (so `cog_orchestrator_sessions`
  can still report affected subagents during crash recovery) until they
  are replaced (auto-replace) or the orchestrator is freed.
- A single orchestrator is not thread-safe. All functions are called from
  the thread that runs the main core's event loop.

## Functions

### `cog_orchestrator_new` / `cog_orchestrator_free`

```c
cog_result_t cog_orchestrator_new(cog_orchestrator_t **out,
                                   const cog_orchestrator_config_t *config);
void cog_orchestrator_free(cog_orchestrator_t *orch);
```

| Return | Condition |
|--------|-----------|
| `COG_OK` | Success; `*out` is a valid orchestrator. |
| `COG_ERROR_NULL_PTR` | `out` or `config` is NULL. |
| `COG_ERROR_INVALID_ARG` | `config->socket_path` is NULL or overlong (≥ 128 bytes). |
| `COG_ERROR_NOMEM` | Allocation failed. |
| `COG_ERROR_IO` | The listener socket could not be created/bound. |

Zero config fields select defaults: `max_aux_cores`=4,
`health_interval_ms`=1000, `unhealthy_threshold`=3,
`default_core_capacity`=8.

### `cog_orchestrator_spawn`

```c
cog_result_t cog_orchestrator_spawn(cog_orchestrator_t *orch,
                                     const char *binary_path,
                                     cog_peer_id_t *out_peer_id);
```

Forks and execs `binary_path` with the socket path as `argv[1]`, waits for
the child to connect (up to the spawn timeout), accepts it, and returns the
assigned peer id.

| Return | Condition |
|--------|-----------|
| `COG_OK` | Child connected; `*out_peer_id` set. |
| `COG_ERROR_NULL_PTR` | `orch`, `binary_path`, or `out_peer_id` is NULL. |
| `COG_ERROR_OVERFLOW` | `max_aux_cores` already reached. |
| `COG_ERROR_IO` | fork/exec failed or the child never connected. |
| `COG_ERROR_NOMEM` | Allocation failed. |

### `cog_orchestrator_stop` / `cog_orchestrator_kill`

```c
cog_result_t cog_orchestrator_stop(cog_orchestrator_t *orch, cog_peer_id_t peer_id);
cog_result_t cog_orchestrator_kill(cog_orchestrator_t *orch, cog_peer_id_t peer_id);
```

`stop` sends `shutdown` and waits for `ack` (up to the ack timeout). If the
core acks it is marked disconnected (health DEAD); if it has already exited
it is reaped, and if it is still alive it is left for `reap_children` /
normal cleanup so `stop` never blocks on a non-exiting child. Otherwise it
is force-killed. Either way the core ends up stopped and `COG_OK` is
returned (the header documents no timeout error for `stop`).

`kill` SIGKILLs a spawned core (or, for a non-spawned/self-connected core,
just closes the connection and marks it dead) and returns `COG_OK`.

| Return | Condition |
|--------|-----------|
| `COG_OK` | Success. |
| `COG_ERROR_NULL_PTR` | `orch` is NULL. |
| `COG_ERROR_NOT_FOUND` | The peer does not exist. |

### `cog_orchestrator_health`

```c
cog_health_t cog_orchestrator_health(const cog_orchestrator_t *orch, cog_peer_id_t peer_id);
```

Returns the computed health, or `COG_HEALTH_UNKNOWN` if `orch` is NULL or
the peer is unknown.

### `cog_orchestrator_list`

```c
cog_result_t cog_orchestrator_list(const cog_orchestrator_t *orch,
                                    cog_core_info_t *out_infos,
                                    size_t max_infos, size_t *out_count);
```

Copies up to `max_infos` core infos and reports the total count in
`*out_count` (which may exceed `max_infos`). `out_infos` may be NULL to
query only the count.

| Return | Condition |
|--------|-----------|
| `COG_OK` | Success. |
| `COG_ERROR_NULL_PTR` | `orch` or `out_count` is NULL. |

### `cog_orchestrator_heartbeat`

```c
cog_result_t cog_orchestrator_heartbeat(cog_orchestrator_t *orch,
                                         cog_peer_id_t peer_id,
                                         size_t active_subagents);
```

Records a heartbeat, sets health to `HEALTHY`, and updates the active
subagent count.

| Return | Condition |
|--------|-----------|
| `COG_OK` | Success. |
| `COG_ERROR_NULL_PTR` | `orch` is NULL. |
| `COG_ERROR_NOT_FOUND` | The peer is unknown. |

### `cog_orchestrator_place`

```c
cog_peer_id_t cog_orchestrator_place(const cog_orchestrator_t *orch, size_t estimated_load);
```

Selects the healthiest core with capacity for `estimated_load`, preferring
`HEALTHY` over `DEGRADED` and, within a health tier, the core with the most
free capacity. Returns `COG_PEER_ID_INVALID` (0) when no remote core can
take the load — the caller then runs the subagent locally (this is where
`force_local` subagents land; `force_local` is handled by the caller, which
does not call `place`). Returns `COG_PEER_ID_INVALID` if `orch` is NULL.

### `cog_orchestrator_shutdown`

```c
cog_result_t cog_orchestrator_shutdown(cog_orchestrator_t *orch, uint32_t timeout_ms);
```

Sends `shutdown` to every live core and waits up to `timeout_ms` for
acknowledgements. Cores that do not ack are force-killed. A core that acks
is marked disconnected (health DEAD); if it has already exited it is
reaped, and if it is still alive it is left for `reap_children` / normal
cleanup so `shutdown` never blocks on a non-exiting child.

| Return | Condition |
|--------|-----------|
| `COG_OK` | Every live core acknowledged. |
| `COG_ERROR_NULL_PTR` | `orch` is NULL. |
| `COG_ERROR_TIMEOUT` | Not every core acknowledged in time (non-ackers are force-killed). |

### `cog_orchestrator_sessions`

```c
cog_result_t cog_orchestrator_sessions(const cog_orchestrator_t *orch,
                                        cog_peer_id_t peer_id,
                                        const char **out_sessions,
                                        size_t max_sessions,
                                        size_t *out_count);
```

Copies up to `max_sessions` session-id pointers (borrowed, owned by the
orchestrator) hosted on the peer and reports the total count.

| Return | Condition |
|--------|-----------|
| `COG_OK` | Success. |
| `COG_ERROR_NULL_PTR` | `orch`, `out_sessions`, or `out_count` is NULL. |
| `COG_ERROR_NOT_FOUND` | The peer is unknown. |

## Private hooks (orchestrator_internal.h)

- `cog_orchestrator_poll(orch, timeout_ms)` — reap, accept, drain, auto-replace.
- `cog_orchestrator_accept(orch, out_peer_id)` — accept one connection.
- `cog_orchestrator_register_session` / `cog_orchestrator_unregister_session`
  — session tracking (idempotent register; NOT_FOUND on missing unregister).
- `cog_orchestrator_set_now` / `cog_orchestrator_now` — clock override.
- `cog_orchestrator_set_spawn_timeout` / `cog_orchestrator_set_ack_timeout`
  — timeout overrides.

## Invariants

- `cog_orchestrator_stop` and `cog_orchestrator_shutdown` never block on a
  child that acknowledged shutdown but has not exited: after an ack they
  reap only if the child has already exited (non-blocking `waitpid`
  `WNOHANG`) and leave a still-running child for `reap_children` / normal
  cleanup. Blocking `waitpid(..., 0)` is used only after `SIGKILL`, which
  is bounded.
- Auto-replace is recovery after actual process exit, never termination of
  a draining core.  A spawned core's slot is only reused after its process
  has actually exited and been reaped (`reap_children`).  An acked-but-alive
  child (disconnected, still running) is skipped while it is alive: its
  pid/binary/connection records stay owned for `reap_children` / normal
  cleanup, and its slot is never reused (which would memset the record and
  orphan the live pid).  Non-spawned/self-connected cores are never
  signalled.
- Every spawned child is reaped before the orchestrator is freed.
- A core is never chosen for placement while `UNKNOWN` or `DEAD`.
- `cog_orchestrator_sessions` on a dead core still returns its sessions
  (crash recovery).
- `cog_orchestrator_free` never SIGKILLs a non-spawned (self-connected)
  core, so a test process that connects to its own orchestrator is safe.
- The socket path file is unlinked on `free`.