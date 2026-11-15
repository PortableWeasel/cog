# Scenario 3: Subagent Spawn → Execute → Status → Result → Parent

## Sequence Diagram

```
Main Core         Orchestrator        Aux Core 1         Subagent
   │                  │                  │                  │
   │ handler decides  │                  │                  │
   │ to spawn subagent│                  │                  │
   │                  │                  │                  │
   │ cog_agent_spawn()│                  │                  │
   │─────────────────>│                  │                  │
   │                  │ cog_orchestrator_place()            │
   │                  │ → peer_id=aux1  │                  │
   │                  │                  │                  │
   │                  │ cog_ipc_send()  │                  │
   │                  │ spawn command   │                  │
   │                  │────────────────>│                  │
   │                  │                  │                  │
   │                  │                  │ create child     │
   │                  │                  │ session with     │
   │                  │                  │ parent_id        │
   │                  │                  │                  │
   │                  │                  │ run subagent     │
   │                  │                  │ event loop       │
   │                  │                  │─────────────────>│
   │                  │                  │                  │
   │                  │                  │                  │ executing...
   │                  │                  │                  │
   │                  │                  │                  │ cog_agent_notify_parent()
   │                  │                  │<─────────────────│
   │                  │                  │                  │
   │ COG_EVENT_SUBAGENT_STATUS           │                  │
   │<────────────────────────────────────│                  │
   │ handler: update UI                  │                  │
   │                  │                  │                  │
   │                  │                  │                  │ task complete
   │                  │                  │                  │
   │                  │                  │                  │ cog_agent_notify_parent()
   │                  │                  │<─────────────────│
   │ COG_EVENT_SUBAGENT_RESULT           │                  │
   │<────────────────────────────────────│                  │
   │ handler: process result             │                  │
   │ transition state  │                  │                  │
```

## Header Coverage

| Step | Function/Event | Header | Status |
|------|---------------|--------|--------|
| Spawn | `cog_agent_spawn()` | `agent.h` | ✅ (6j) |
| Placement | `cog_orchestrator_place()` | `orchestrator.h` | ✅ (6i) |
| Send spawn | `cog_ipc_endpoint_send()` | `ipc.h` | ✅ |
| Child session | `cog_session_set_parent()` | `session.h` | ✅ (6h) |
| Notify parent | `cog_agent_notify_parent()` | `agent.h` | ✅ (6j) |
| Status event | `COG_EVENT_SUBAGENT_STATUS` | `event.h` | ✅ (6h) |
| Result event | `COG_EVENT_SUBAGENT_RESULT` | `event.h` | ✅ |
| Get result | `cog_agent_result()` | `agent.h` | ✅ (6j) |
| Cancel | `cog_agent_cancel()` | `agent.h` | ✅ (6j) |
| Session routing | `cog_ipc_send_to_session()` | `ipc.h` | ✅ (6k) |

## Gaps Found

1. **Session→peer mapping registration** — when a subagent spawns on an aux core, someone must call `cog_ipc_register_session()` to map the session id to the aux core's peer id. **Who does this?** The orchestrator should register the mapping when it spawns the subagent. **Fix: document in orchestrator.h that spawn() registers the session mapping.**

2. **Result routing** — when the subagent calls `cog_agent_notify_parent()`, the message goes to the parent *session*, not the parent *core*. The IPC layer needs to know which peer hosts the parent session. **This works if the main core registers its own sessions at startup.** Document this requirement.

3. **Timeout** — `cog_agent_config_t.timeout_ms` exists but there's no event for timeout. **Fix: add COG_EVENT_SUBAGENT_TIMEOUT or treat it as SUBAGENT_RESULT with error status.**

## Verdict

Headers cover the flow. Two clarifications needed:
- Document that orchestrator registers session→peer mappings on spawn
- Document that the main core registers its own sessions at startup
