# Scenario 7: Aux Core Crash → Detection → Recovery

## Sequence Diagram

```
Main Core         Orchestrator        Aux Core 2        Subagent S1    Subagent S2
   │                  │                  │                  │               │
   │                  │                  │ running S1, S2   │               │
   │                  │                  │                  │               │
   │                  │                  │ CRASH (segfault) │               │
   │                  │                  │                  │               │
   │                  │ heartbeat timeout│                  │               │
   │                  │ (no heartbeat    │                  │               │
   │                  │  for threshold)  │                  │               │
   │                  │                  │                  │               │
   │                  │ mark DEAD        │                  │               │
   │                  │                  │                  │               │
   │ COG_EVENT_IPC_DISCONNECTED           │                  │               │
   │<─────────────────│                  │                  │               │
   │                  │                  │                  │               │
   │ handler:         │                  │                  │               │
   │ - find affected  │                  │                  │               │
   │   subagents S1,S2│                  │                  │               │
   │ - mark FAILED    │                  │                  │               │
   │ - notify parents │                  │                  │               │
   │                  │                  │                  │               │
   │                  │ optionally:      │                  │               │
   │                  │ cog_orchestrator_spawn()             │               │
   │                  │ (replacement)    │                  │               │
```

## Header Coverage

| Step | Function/Event | Header | Status |
|------|---------------|--------|--------|
| Health check | `cog_orchestrator_health()` | `orchestrator.h` | ✅ (6i) |
| Heartbeat | `cog_orchestrator_heartbeat()` | `orchestrator.h` | ✅ (6i) |
| Dead status | `COG_HEALTH_DEAD` | `orchestrator.h` | ✅ (6i) |
| Disconnect event | `COG_EVENT_IPC_DISCONNECTED` | `event.h` | ✅ (6h) |
| Find subagents | `cog_session_parent_id()` | `session.h` | ✅ (6h) |
| Mark failed | `cog_agent_status()` | `agent.h` | ✅ (6j) |
| Respawn | `cog_orchestrator_spawn()` | `orchestrator.h` | ✅ (6i) |

## Gaps Found

1. **Finding affected subagents** — the handler needs to enumerate all sessions whose parent chain leads to the crashed core. But there's no `cog_session_list_children()` or `cog_core_find_sessions_by_peer()`. **Fix: add `cog_orchestrator_sessions(orch, peer_id, out_sessions, out_count)` or maintain a session registry in the core.**

2. **Subagent state persistence** — if a subagent was mid-task when the core crashed, its state is lost. **Design decision:** should the orchestrator checkpoint subagent state? For now, document that crashed subagents are marked FAILED and must be restarted from scratch.

3. **Replacement core** — spawning a replacement is optional. The orchestrator should have a policy: auto-replace dead cores, or wait for manual intervention. **Fix: add `auto_replace` flag to `cog_orchestrator_config_t`.**

## Verdict

Headers cover the flow. One amendment needed:
- Add `cog_orchestrator_sessions()` to enumerate sessions on a peer
- Add `auto_replace` to orchestrator config
