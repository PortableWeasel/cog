# Scenario 10: Fan-out/Fan-in Across Aux Cores

## Sequence Diagram

```
Main Core         Orchestrator        Aux Core 1    Aux Core 2    Aux Core 3
   │                  │                  │              │              │
   │ handler: fan-out │                  │              │              │
   │ same task to 3   │                  │              │              │
   │ aux cores        │                  │              │              │
   │                  │                  │              │              │
   │ cog_agent_spawn() x3                │              │              │
   │ (same task)      │                  │              │              │
   │─────────────────>│                  │              │              │
   │                  │ place S1→aux1    │              │              │
   │                  │ place S2→aux2    │              │              │
   │                  │ place S3→aux3    │              │              │
   │                  │                  │              │              │
   │                  │ spawn commands   │              │              │
   │                  │─────────────────>│              │              │
   │                  │───────────────────────────────>│              │
   │                  │─────────────────────────────────────────────>│
   │                  │                  │              │              │
   │                  │                  │ S1 running   │ S2 running   │ S3 running
   │                  │                  │              │              │
   │                  │                  │              │ S2 completes │
   │                  │                  │              │              │
   │ COG_EVENT_SUBAGENT_RESULT from S2   │              │              │
   │<────────────────────────────────────────────────────│              │
   │                  │                  │              │              │
   │ handler:         │                  │              │              │
   │ - record S2 win  │                  │              │              │
   │ - cancel S1, S3  │                  │              │              │
   │                  │                  │              │              │
   │ cog_agent_cancel(S1)                │              │              │
   │ cog_agent_cancel(S3)                │              │              │
   │─────────────────>│                  │              │              │
   │                  │ cancel S1        │              │              │
   │                  │─────────────────>│              │              │
   │                  │ cancel S3        │              │              │
   │                  │─────────────────────────────────────────────>│
   │                  │                  │              │              │
   │                  │                  │ S1 cancelled │              │ S3 cancelled
   │                  │                  │              │              │
   │ transition state │                  │              │              │
   │ with S2's result │                  │              │              │
```

## Header Coverage

| Step | Function | Header | Status |
|------|----------|--------|--------|
| Spawn multiple | `cog_agent_spawn()` | `agent.h` | ✅ (6j) |
| Placement | `cog_orchestrator_place()` | `orchestrator.h` | ✅ (6i) |
| Cancel | `cog_agent_cancel()` | `agent.h` | ✅ (6j) |
| Result event | `COG_EVENT_SUBAGENT_RESULT` | `event.h` | ✅ |
| Broadcast cancel | `cog_ipc_broadcast()` | `ipc.h` | ✅ (6k) |

## Gaps Found → ALL FIXED

1. ✅ **Late result handling** — added `cog_agent_is_cancelled()`
2. ✅ **Fan-out grouping** — added `group_id` to `cog_agent_config_t`
3. **Cancellation** — best-effort; handlers check status before processing results

## Verdict

✅ All gaps fixed. Headers fully cover the flow.
