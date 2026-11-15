# Scenario 5: Graceful Shutdown with In-Flight Work

## Sequence Diagram

```
Signal          Core             Net             Provider         IPC
  │               │               │                │               │
  │ SIGINT        │               │                │               │
  │──────────────>│               │                │               │
  │               │               │                │               │
  │               │ cog_core_stop()│               │               │
  │               │ (sets flag)   │                │               │
  │               │               │                │               │
  │               │ finish current│                │               │
  │               │ event dispatch│                │               │
  │               │               │                │               │
  │               │ COG_EVENT_SHUTDOWN             │               │
  │               │ enqueue       │                │               │
  │               │               │                │               │
  │               │ shutdown handler:              │               │
  │               │               │                │               │
  │               │ cog_net_cancel() for each      │               │
  │               │ in-flight request              │               │
  │               │──────────────>│                │               │
  │               │               │                │               │
  │               │ cog_ipc_broadcast() shutdown   │               │
  │               │ to all subagents               │               │
  │               │───────────────────────────────────────────────>│
  │               │               │                │               │
  │               │ wait for acks │                │               │
  │               │ (with timeout)│                │               │
  │               │               │                │               │
  │               │ cog_provider_disconnect()      │               │
  │               │ for each provider              │               │
  │               │───────────────────────────────>│               │
  │               │               │                │               │
  │               │ cog_ipc_endpoint_disconnect()  │               │
  │               │ cog_ipc_endpoint_free()        │               │
  │               │───────────────────────────────────────────────>│
  │               │               │                │               │
  │               │ cog_net_free()│                │               │
  │               │──────────────>│                │               │
  │               │               │                │               │
  │               │ cog_session_free()             │               │
  │               │ cog_obs_free() │               │               │
  │               │ cog_config_tree_free()         │               │
  │               │ cog_core_free()│               │               │
  │               │               │                │               │
  │               │ return from cog_core_run()     │               │
  │               │               │                │               │
  │               │ main() returns 0               │               │
```

## Header Coverage

| Step | Function | Header | Status |
|------|----------|--------|--------|
| Signal stop | `cog_core_stop()` | `core.h` | ✅ |
| Shutdown event | `COG_EVENT_SHUTDOWN` | `event.h` | ✅ |
| Cancel requests | `cog_net_cancel()` | `net.h` | ✅ |
| Broadcast | `cog_ipc_broadcast()` | `ipc.h` | ✅ (6k) |
| Disconnect provider | `cog_provider_disconnect()` | `provider_vtable.h` | ✅ |
| Close IPC | `cog_ipc_endpoint_disconnect/free()` | `ipc.h` | ✅ |
| Free net | `cog_net_free()` | `net.h` | ✅ |
| Free session | `cog_session_free()` | `session.h` | ✅ |
| Free core | `cog_core_free()` | `core.h` | ✅ |

## Gaps Found

1. **Subagent shutdown timeout** — the scenario says "wait for acks with timeout" but there's no `cog_orchestrator_stop_all()` or timeout parameter. **Fix: add `cog_orchestrator_shutdown(orch, timeout_ms)` that stops all aux cores with a deadline.**

2. **In-flight tracking** — to cancel all in-flight requests, the core needs to know what's active. `cog_net_active_count()` gives a count but not the ids. **Fix: add `cog_net_active_ids(ctx, out_ids, max_ids, out_count)`.**

3. **Observability of shutdown** — the shutdown sequence should be logged. But there's no way to pass obs context to the shutdown handler. **Same gap as cold start: obs context needs to flow into the core config.**

## Verdict

Headers cover the shutdown sequence. Two amendments needed:
- Add `cog_orchestrator_shutdown()` with timeout
- Add `cog_net_active_ids()` for bulk cancellation
