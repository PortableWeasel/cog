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

## Gaps Found

1. **Output buffering** — the core needs to buffer output for a disconnected UI. But there's no `cog_core_buffer_output()` or buffer API. **Fix: add a `cog_output_buffer_t` type and `cog_core_buffer_write()` / `cog_core_buffer_flush()` functions, or document that the application manages buffering.**

2. **UI identity** — when the UI reconnects, how does the core know it's the same UI? The peer id is new (new connection). The UI must identify itself with a session id or client token. **Fix: add `cog_ipc_endpoint_send_identify()` or include a client token in the connect handshake.**

3. **Buffer limits** — if the UI is disconnected for a long time, the buffer could grow unbounded. **Fix: add a `max_buffer_size` config and drop oldest chunks when full.**

## Verdict

Headers cover the flow but need a buffering strategy. This is a significant design decision:
- Option A: Core manages output buffering (new API)
- Option B: Application manages buffering (document the pattern)
- Option C: Don't buffer — UI re-requests full response on reconnect
