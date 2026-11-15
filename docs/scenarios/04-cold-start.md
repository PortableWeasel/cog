# Scenario 4: Cold Start → Init → First Event

## Sequence Diagram

```
main()           Config           IPC              Orchestrator      Core
  │                │               │                  │               │
  │ parse args     │               │                  │               │
  │                │               │                  │               │
  │ cog_config_tree_new()          │                  │               │
  │───────────────>│               │                  │               │
  │                │               │                  │               │
  │ load config    │               │                  │               │
  │ from file/CLI  │               │                  │               │
  │ cog_config_begin()             │                  │               │
  │ cog_config_set() for each      │                  │               │
  │ cog_config_commit()            │                  │               │
  │───────────────>│               │                  │               │
  │                │               │                  │               │
  │ cog_obs_new()  │               │                  │               │
  │ (observability)│               │                  │               │
  │                │               │                  │               │
  │ cog_ipc_endpoint_new()         │                  │               │
  │ cog_ipc_endpoint_listen()      │                  │               │
  │───────────────>│               │                  │               │
  │                │               │                  │               │
  │ cog_orchestrator_new()         │                  │               │
  │ cog_orchestrator_start()       │                  │               │
  │────────────────────────────────>│                  │               │
  │                │               │                  │               │
  │ cog_net_new()  │               │                  │               │
  │ (network)      │               │                  │               │
  │                │               │                  │               │
  │ cog_session_new()              │                  │               │
  │ (root session) │               │                  │               │
  │                │               │                  │               │
  │ cog_core_new() │               │                  │               │
  │ (config: session, dispatch,    │                  │               │
  │  tools, ipc_endpoint)          │                  │               │
  │───────────────────────────────────────────────────>│               │
  │                │               │                  │               │
  │ cog_core_run() │               │                  │               │
  │───────────────────────────────────────────────────>│               │
  │                │               │                  │               │
  │                │               │                  │               │ event loop
  │                │               │                  │               │ waiting...
  │                │               │                  │               │
  │ UI connects    │               │                  │               │
  │───────────────────────────────>│                  │               │
  │                │               │ accept           │               │
  │                │               │─────────────────>│               │
  │                │               │                  │               │ COG_EVENT_IPC_CONNECTED
  │                │               │                  │               │
  │ First user input               │                  │               │
  │───────────────────────────────>│                  │               │
  │                │               │ forward          │               │
  │                │               │─────────────────>│               │
  │                │               │                  │               │ COG_EVENT_USER_INPUT
  │                │               │                  │               │ dispatch...
```

## Header Coverage

| Step | Function | Header | Status |
|------|----------|--------|--------|
| Config tree | `cog_config_tree_new()` | `config.h` | ✅ |
| Config transaction | `cog_config_begin/set/commit()` | `config.h` | ✅ |
| Observability | `cog_obs_new()` | `obs.h` | ✅ |
| IPC listen | `cog_ipc_endpoint_listen()` | `ipc.h` | ✅ |
| Orchestrator | `cog_orchestrator_new()` | `orchestrator.h` | ✅ (6i) |
| Network | `cog_net_new()` | `net.h` | ✅ |
| Session | `cog_session_new()` | `session.h` | ✅ |
| Core init | `cog_core_new()` | `core.h` | ✅ |
| Core run | `cog_core_run()` | `core.h` | ✅ |
| IPC accept | `cog_ipc_endpoint_accept()` | `ipc.h` | ✅ |
| Connected event | `COG_EVENT_IPC_CONNECTED` | `event.h` | ✅ (6h) |

## Gaps Found → ALL FIXED

1. ✅ **Obs context in config** — added `cog_obs_t *obs` to `cog_core_config_t`
2. **Config → Provider wiring** — application handles this via `cog_config_get()` and endpoint table construction
3. **Init order** — documented: all referenced objects must be initialized before `cog_core_new()`

## Verdict

✅ All gaps fixed. Headers fully cover the flow.
