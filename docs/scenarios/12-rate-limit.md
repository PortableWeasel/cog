# Scenario 12: Provider Rate Limit → Backoff → Queue

## Sequence Diagram

```
Core             Provider          cog-net           LLM API
  │                 │                 │                 │
  │ cog_provider_send()               │                 │
  │────────────────>│                 │                 │
  │                 │ cog_net_request()│                │
  │                 │────────────────>│                 │
  │                 │                 │ HTTP POST       │
  │                 │                 │────────────────>│
  │                 │                 │                 │
  │                 │                 │ HTTP 429        │
  │                 │                 │ Retry-After: 30 │
  │                 │                 │<────────────────│
  │                 │                 │                 │
  │                 │ COG_EVENT_PROVIDER_ERROR           │
  │                 │ (rate_limited)  │                 │
  │<────────────────│                 │                 │
  │                 │                 │                 │
  │ handler:        │                 │                 │
  │ - parse Retry-After               │                 │
  │ - schedule retry after 30s        │                 │
  │ - queue subsequent requests       │                 │
  │ - notify UI of rate limit         │                 │
  │                 │                 │                 │
  │ ... 30s ...     │                 │                 │
  │                 │                 │                 │
  │ retry: cog_provider_send()        │                 │
  │────────────────>│                 │                 │
  │                 │ cog_net_request()│                │
  │                 │────────────────>│                 │
  │                 │                 │ HTTP POST       │
  │                 │                 │────────────────>│
  │                 │                 │                 │
  │                 │                 │ HTTP 200        │
  │                 │                 │<────────────────│
  │                 │                 │                 │
  │                 │ streaming...    │                 │
```

## Header Coverage

| Step | Function/Event | Header | Status |
|------|---------------|--------|--------|
| Send request | `cog_provider_send()` | `provider_vtable.h` | ✅ |
| HTTP request | `cog_net_request()` | `net.h` | ✅ |
| Error event | `COG_EVENT_PROVIDER_ERROR` | `event.h` | ✅ (6h) |
| Response status | `cog_net_response_t.http_status` | `net.h` | ✅ |
| Retry config | `cog_net_config_t.max_retries` | `net.h` | ✅ |
| Backoff config | `cog_net_config_t.retry_backoff_ms` | `net.h` | ✅ |

## Gaps Found

1. **Retry-After header access** — the response doesn't expose HTTP headers. The handler can't read `Retry-After: 30`. **Fix: add `const char *headers` to `cog_net_response_t`.**

2. **Rate limit error detail** — `COG_EVENT_PROVIDER_ERROR` doesn't specify *what* kind of error. The handler needs to know it's a rate limit (retryable) vs auth error (not retryable). **Fix: add `cog_provider_error_t` struct with error code, message, and retryable flag.**

3. **Request queuing** — during backoff, subsequent requests should be queued. But there's no queue API in cog-net. **Fix: add `cog_net_queue_request()` that queues instead of executing immediately, or document that the application manages queuing.**

4. **Exponential backoff** — the config has a single `retry_backoff_ms` but no multiplier. **Fix: add `retry_backoff_multiplier` to config, or document that the application implements backoff.**

## Verdict

Headers cover the flow but need error detail improvements:
- Add `headers` to `cog_net_response_t`
- Add `cog_provider_error_t` with retryable flag
- Consider request queuing strategy
