# cog-orchestrator {#page_cog_orchestrator}

## What it does

`cog-orchestrator` supervises auxiliary Cog core processes. It provides:

- **spawn / stop / kill** for child cores,
- **health tracking** from heartbeats,
- **placement** decisions for subagents,
- **session ownership** publication and lookup,
- **graceful shutdown** with a bounded deadline,
- **handoff messages** for migrating sessions between cores.

Like every other module, it is a nonblocking state machine: you call
`cog_orchestrator_service` and it advances connections, heartbeats, kills, and
reaping within a bounded quantum.

Headers: `orchestrator.h`.

## Key ideas

**The orchestrator owns the listener.** `socket_path` is bound by the
orchestrator; children connect back to it. This means the application needs
no separate IPC setup for child supervision.

**Spawn is asynchronous.** `cog_orchestrator_spawn` forks and execs without
waiting for the child to connect or identify itself. The returned peer id is
reserved immediately, but the core's health stays `COG_HEALTH_UNKNOWN` until
`service` completes the handshake. Do not admit work on an unknown core.

**Shutdown is a state, not a function return.** `stop` and
`request_shutdown` admit intent; `service` drives acknowledgement, `SIGKILL`
if needed, and nonblocking `waitpid`. `free` is only safe after all owned
children have been reaped.

**Health has four levels.** `UNKNOWN`, `HEALTHY`, `DEGRADED` (one missed
heartbeat), and `DEAD` (threshold missed). `auto_replace` spawns idle
replacement capacity, never a replay of the dead core's tools.

**Placement is advisory and deterministic.** `cog_orchestrator_place` picks a
core for a new subagent based on health and capacity. The caller still owns
the policy decision.

## Getting started: spawn, supervise, shut down

```c
#include <cog/orchestrator.h>

#include <stdint.h>

int supervise(const char *socket_path, const char *core_binary);

int supervise(const char *socket_path, const char *core_binary)
{
    cog_orchestrator_config_t config = {0};
    cog_orchestrator_t *orch = NULL;
    cog_peer_id_t peer = 0;
    uint64_t now = 0;
    uint64_t deadline = 0;
    bool shutdown_done = false;
    int status = 1;
    int i;

    config.socket_path = socket_path;
    config.max_aux_cores = 4;
    config.health_interval_ms = 1000;
    config.unhealthy_threshold = 3;
    config.default_core_capacity = 8;

    if (cog_orchestrator_new(&orch, &config) != COG_OK) {
        return 1;
    }

    if (cog_orchestrator_spawn(orch, core_binary, &peer) != COG_OK) {
        goto done;
    }

    /* There is no hidden thread: drive connection, heartbeat, and reap. */
    for (i = 0; i < 10; i++) {
        (void)cog_orchestrator_service(orch, now, &shutdown_done, &deadline);
        if (cog_orchestrator_health(orch, peer) == COG_HEALTH_HEALTHY) {
            status = 0;
        }
        now += 1000;
    }

    if (cog_orchestrator_request_shutdown(orch, now, 1000) != COG_OK) {
        goto done;
    }
    while (cog_orchestrator_service(orch, now, &shutdown_done, &deadline) ==
               COG_OK &&
           !shutdown_done) {
        now += 100;
    }

done:
    cog_orchestrator_free(orch);
    return status;
}
```

This example polls in a tight loop so it stays short. In a real host the
`service` call belongs in the same bounded loop as the core, with
`deadline` feeding the wait.

## Placement and session ownership

Two related tables live in the orchestrator:

- **Placement.** `cog_orchestrator_place` answers "which core should take
  this subagent?" from the current health/capacity snapshot. It does not
  spawn or move anything by itself.
- **Ownership.** `cog_orchestrator_publish_owner`,
  `cog_orchestrator_session_owner`, and `cog_orchestrator_forget_owner`
  record which core durably owns a session. Ownership is backed by the
  configured store and a process-incarnation tuple, not by a socket address,
  so a reconnect cannot steal a live session.

`cog_orchestrator_sessions` lists the sessions a core currently owns.

## Handoff

`cog_orchestrator_handoff_encode` and `cog_orchestrator_handoff_decode`
serialize a migration message: the session id, source and destination
owners, and a kind describing the operation. The receiving core loads the
exact checkpoint from the shared store before it accepts work. The
orchestrator drives transport, not policy.

## Beyond the basics

- **Shutdown deadline.** `shutdown_timeout_ms` is the per-child grace period
  before `SIGKILL`. `request_shutdown` never extends an earlier deadline.
- **Bounded work.** `service_quantum` caps how much connection, heartbeat,
  shutdown, and reap work one `service` call performs, even with continuous
  traffic.
- **No foreign PIDs.** The orchestrator signals only PIDs it spawned and
  owns.
- **Aux crash.** A dead core is detected by health thresholds. Its sessions
  stay failed/frozen until an explicit recovery or migration; tools are never
  replayed automatically.

## See also

- @ref page_cog_ipc — the transport children use
- @ref page_cog_core — what a child core runs
- @ref page_cog_obs — health and connection events
- `docs/scenarios/07-aux-crash.md` and `09-subagent-messaging.md`
