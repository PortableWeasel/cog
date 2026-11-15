# Scenario 2: Network Stall → Detection → Recovery → Resume

## Sequence Diagram

```
Provider         cog-net           Main Core           UI
   │                │                  │                │
   │ streaming...   │                  │                │
   │───────────────>│                  │                │
   │                │ cog_net_poll()   │                │
   │                │─────────────────>│                │
   │                │ chunk            │                │
   │                │─────────────────>│                │
   │                │                  │──>UI (stream)──>│
   │                │                  │                │
   │ NETWORK DROPS  │                  │                │
   │                │                  │                │
   │  ... silence ...                  │                │
   │                │                  │                │
   │                │ stall_timeout_ms │                │
   │                │ fires            │                │
   │                │                  │                │
   │                │ COG_EVENT_NETWORK_STALL            │
   │                │─────────────────>│                │
   │                │                  │                │
   │                │                  │ handler:       │
   │                │                  │ - notify UI    │
   │                │                  │ - start retry  │
   │                │                  │ - → RETRYING   │
   │                │                  │──>status──────>│
   │                │                  │                │
   │                │ retry 1 (fails)  │                │
   │                │ retry 2 (fails)  │                │
   │                │  ... 30s ...     │                │
   │                │                  │                │
   │ NETWORK BACK   │                  │                │
   │                │                  │                │
   │                │ COG_EVENT_NETWORK_RECOVERED        │
   │                │─────────────────>│                │
   │                │                  │                │
   │                │                  │ handler:       │
   │                │                  │ - reconnect    │
   │                │                  │ - resend req   │
   │                │                  │ - → AWAITING_LLM│
   │                │                  │                │
   │                │ streaming resumes│                │
   │                │─────────────────>│                │
   │                │                  │──>UI (stream)──>│
```

## Header Coverage

| Step | Function/Event | Header | Status |
|------|---------------|--------|--------|
| Stall detection | `cog_net_poll()` timeout | `net.h` | ✅ |
| Stall event | `COG_EVENT_NETWORK_STALL` | `event.h` | ✅ (6h) |
| Recovery event | `COG_EVENT_NETWORK_RECOVERED` | `event.h` | ✅ (6h) |
| Retry config | `cog_net_config_t.max_retries` | `net.h` | ✅ |
| Backoff config | `cog_net_config_t.retry_backoff_ms` | `net.h` | ✅ |
| Reconnect | `cog_provider_connect()` | `provider_vtable.h` | ✅ |
| Resend | `cog_provider_send()` | `provider_vtable.h` | ✅ |
| UI notification | `cog_ipc_endpoint_send()` | `ipc.h` | ✅ |
| Status event | `COG_EVENT_STATUS_UPDATE` | `event.h` | ✅ (6h) |

## Gaps Found → FIXED

1. ✅ **Retry-After header** — added `headers` field to `cog_net_response_t`
2. **Retry initiation** — cog-net retries autonomously up to `max_retries`, then emits `COG_EVENT_NETWORK_STALL`. State machine handles higher-level recovery.
3. **Resume vs restart** — provider restarts the request. Session history is intact.

## Verdict

✅ Gap fixed. Headers fully cover the flow.
