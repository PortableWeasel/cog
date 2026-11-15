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

## Gaps Found

1. **Who initiates retry?** The contract says cog-net has `max_retries`, but doesn't specify whether the net layer retries autonomously or the state machine drives it. **Decision needed:** cog-net should retry autonomously up to `max_retries`, then emit `COG_EVENT_NETWORK_STALL`. The state machine handles higher-level recovery (reconnect, resend).

2. **Resume vs restart** — after recovery, does the provider resume the same request or start over? The contract doesn't say. **Decision needed:** for simplicity, the provider restarts the request. The session history is intact, so the LLM sees the full conversation. The UI may see duplicate content if the previous partial response was already displayed.

3. **Retry-After header** — HTTP 429 (rate limit) includes a `Retry-After` header. The net layer should respect this. **Gap:** `cog_net_response_t` doesn't expose headers. **Fix: add `const char *headers` field.**

## Verdict

Headers cover the flow. One amendment needed:
- Add `const char *headers` to `cog_net_response_t` for Retry-After parsing.
