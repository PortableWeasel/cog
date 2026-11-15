# Scenario 11: UI Disconnect → Buffer → Reconnect → Replay

## Sequence Diagram

```
UI Process        IPC Fabric         Main Core           Provider
   │                  │                  │                  │
   │                  │                  │ streaming LLM    │
   │                  │                  │ response to UI   │
   │<─────────────────│<─────────────────│                  │
   │                  │                  │                  │
   │ CRASH / DISCONNECT                 │                  │
   │                  │                  │                  │
   │                  │ COG_EVENT_IPC_DISCONNECTED           │
   │                  │─────────────────>│                  │
   │                  │                  │                  │
   │                  │                  │ handler:         │
   │                  │                  │ - continue LLM   │
   │                  │                  │ - buffer output  │
   │                  │                  │ - don't waste    │
   │                  │                  │   tokens         │
   │                  │                  │                  │
   │                  │                  │ LLM continues    │
   │                  │                  │ chunks buffered  │
   │                  │                  │                  │
   │ RECONNECT        │                  │                  │
   │                  │                  │                  │
   │ cog_ipc_endpoint_connect()          │                  │
   │─────────────────>│                  │                  │
   │                  │ accept           │                  │
   │                  │─────────────────>│                  │
   │                  │                  │                  │
   │                  │ COG_EVENT_IPC_CONNECTED              │
   │                  │─────────────────>│                  │
   │                  │                  │                  │
   │                  │                  │ handler:         │
   │                  │                  │ - identify UI    │
   │                  │                  │ - send buffered  │
   │                  │                  │   chunks         │
   │<─────────────────│<─────────────────│                  │
   │                  │                  │                  │
   │ render buffered  │                  │                  │
   │ + live output    │                  │                  │
```

## Header Coverage

| Step | Function/Event | Header | Status |
|------|---------------|--------|--------|
| Disconnect event | `COG_EVENT_IPC_DISCONNECTED` | `event.h` | ✅ (6h) |
| Reconnect event | `COG_EVENT_IPC_CONNECTED` | `event.h` | ✅ (6h) |
| IPC connect | `cog_ipc_endpoint_connect()` | `ipc.h` | ✅ |
| IPC accept | `cog_ipc_endpoint_accept()` | `ipc.h` | ✅ |
| Send to UI | `cog_ipc_endpoint_send()` | `ipc.h` | ✅ |
| Session identify | `cog_ipc_endpoint_get_peer_id()` | `ipc.h` | ✅ |

## Gaps Found → DESIGN DECISIONS

1. **Output buffering** — application manages buffering; core continues processing during UI disconnect
2. **UI identity** — UI identifies itself via session id in the connect handshake
3. **Buffer limits** — application enforces limits; drop oldest when full

## Verdict

✅ Design decisions documented. Headers cover the flow with application-managed buffering.
