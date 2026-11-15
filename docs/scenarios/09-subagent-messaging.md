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

## Gaps Found

1. **Message deserialization** — the IPC layer delivers raw bytes. The handler must deserialize the message into the appropriate type. The contract doesn't specify a serialization format. **Decision needed:** use a simple length-prefixed format with a type tag, or let the application define the serialization. The `cog_event_ipc_t` struct carries `msg_type` and `msg_payload` — the application interprets the payload based on `msg_type`.

2. **Session discovery** — how does subagent A know subagent B's session id? The spawn API returns an agent id, not a session id. **Fix: add `cog_agent_session_id()` to get the session id for an agent.**

3. **Bidirectional routing** — both aux cores need to register their sessions with the IPC fabric. The main core (listener) maintains the mapping. **Document that aux cores register sessions on connect.**

## Verdict

Headers cover the flow. One amendment needed:
- Add `cog_agent_session_id()` accessor to `agent.h`
