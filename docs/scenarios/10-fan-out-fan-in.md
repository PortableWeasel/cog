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

## Gaps Found

1. **Late result handling** — after S2 wins, S1 and S3 are cancelled. But if S1's result arrives before the cancel takes effect, the handler must discard it. **Fix: document that handlers must check agent status before processing results. Or add `cog_agent_is_cancelled()` check.**

2. **Fan-out grouping** — there's no way to say "these 3 agents are part of the same fan-out group." The handler must track this manually. **Fix: add `cog_agent_group_id()` or a `cog_agent_spawn_group()` function.**

3. **Cancellation acknowledgment** — after sending cancel, the main core doesn't wait for confirmation. The aux cores may still be running. **Document that cancellation is best-effort.**

## Verdict

Headers cover the flow. One amendment needed:
- Add `cog_agent_is_cancelled()` for late result filtering
- Document that cancellation is best-effort
