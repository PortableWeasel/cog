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

## Gaps Found

1. **Init order dependency** — the orchestrator needs the IPC socket to already exist. The core needs the session, dispatch table, and IPC endpoint. The config must be committed before the core reads it. **This is documented in the scenario but not enforced by the headers.** Add a note to `cog_core_new()`: "The caller must ensure all referenced objects (session, dispatch table, tool table, ipc_endpoint) are fully initialized before calling this function."

2. **Config → Provider wiring** — after config commit, the provider registry reads `providers.*` to build the endpoint table. But there's no function that bridges config → provider. **Fix: add `cog_provider_registry_from_config()` or document that the application does this.**

3. **Observability wiring** — every subsystem should log through cog-obs. But there's no way to pass the obs context to cog-core, cog-net, etc. **Fix: add `cog_obs_t *obs` to relevant config structs, or document that logging is done via a global (which violates "no global state").**

## Verdict

Headers cover the init sequence. Two design decisions needed:
- How does config flow into the provider registry?
- How does the observability context flow into subsystems?
