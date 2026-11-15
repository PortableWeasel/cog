# Scenario 9: Subagent-to-Subagent Messaging Across Cores

## Sequence Diagram

```
Main Core         Aux Core 1         Aux Core 2        Subagent A    Subagent B
   │                  │                  │                  │             │
   │ spawn A on aux1  │                  │                  │             │
   │ spawn B on aux2  │                  │                  │             │
   │                  │                  │                  │             │
   │ register sessions│                  │                  │             │
   │ A→aux1, B→aux2   │                  │                  │             │
   │                  │                  │                  │             │
   │                  │                  │                  │ A needs data│
   │                  │                  │                  │ from B      │
   │                  │                  │                  │             │
   │                  │                  │                  │ cog_ipc_send_to_session()
   │                  │                  │                  │ (B_session_id, request)
   │                  │                  │                  │────────────>│ (via IPC)
   │                  │                  │                  │             │
   │                  │                  │                  │             │
   │                  │  IPC route:      │                  │             │
   │                  │  A's message     │                  │             │
   │                  │  to B's session  │                  │             │
   │                  │─────────────────>│                  │             │
   │                  │                  │                  │             │
   │                  │                  │ COG_EVENT_IPC_MESSAGE          │
   │                  │                  │ for session B    │             │
   │                  │                  │                  │             │
   │                  │                  │ dispatch to B    │             │
   │                  │                  │─────────────────>│             │
   │                  │                  │                  │             │
   │                  │                  │                  │ B processes │
   │                  │                  │                  │ request     │
   │                  │                  │                  │             │
   │                  │                  │                  │ B sends response
   │                  │                  │                  │ cog_ipc_send_to_session()
   │                  │                  │                  │ (A_session_id, response)
   │                  │                  │<─────────────────│             │
   │                  │<─────────────────│                  │             │
   │                  │                  │                  │             │
   │                  │ dispatch to A    │                  │             │
   │                  │─────────────────>│                  │             │
   │                  │                  │                  │ A receives  │
   │                  │                  │                  │ response    │
```

## Header Coverage

| Step | Function | Header | Status |
|------|----------|--------|--------|
| Session routing | `cog_ipc_send_to_session()` | `ipc.h` | ✅ (6k) |
| Register mapping | `cog_ipc_register_session()` | `ipc.h` | ✅ (6k) |
| IPC message event | `COG_EVENT_IPC_MESSAGE` | `event.h` | ✅ (6h) |
| IPC payload | `cog_event_ipc_t` | `event.h` | ✅ (6h) |
| Unregister | `cog_ipc_unregister_session()` | `ipc.h` | ✅ (6k) |

## Gaps Found → FIXED

1. ✅ **Session discovery** — added `cog_agent_session_id()` to `agent.h`
2. **Message deserialization** — `cog_event_ipc_t` carries `msg_type` and `msg_payload`; application interprets based on type
3. **Bidirectional routing** — aux cores register sessions on connect via `cog_ipc_register_session()`

## Verdict

✅ Gap fixed. Headers fully cover the flow.
