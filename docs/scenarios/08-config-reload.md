# Scenario 8: Hot Config Reload

## Sequence Diagram

```
Admin           Config Tree        Validators        Core            Provider
  │                 │                 │               │                │
  │ edit config     │                 │               │                │
  │ file            │                 │               │                │
  │                 │                 │               │                │
  │ cog_config_begin()                │               │                │
  │────────────────>│                 │               │                │
  │                 │                 │               │                │
  │ cog_config_set() for each change  │               │                │
  │────────────────>│                 │               │                │
  │                 │                 │               │                │
  │ cog_config_commit()               │               │                │
  │────────────────>│                 │               │                │
  │                 │                 │               │                │
  │                 │ run validators  │               │                │
  │                 │────────────────>│               │                │
  │                 │                 │               │                │
  │                 │ validator: check provider URLs   │                │
  │                 │ validator: check model ids       │                │
  │                 │ validator: check credentials     │                │
  │                 │                 │               │                │
  │                 │ PASS            │               │                │
  │                 │<────────────────│               │                │
  │                 │                 │               │                │
  │                 │ atomic swap     │               │                │
  │                 │ candidate→running               │                │
  │                 │                 │               │                │
  │                 │ notify listeners│               │                │
  │                 │──────────────────────────────>│                │
  │                 │                 │               │                │
  │                 │                 │               │ read new config│
  │                 │                 │               │ rebuild endpoint│
  │                 │                 │               │ table          │
  │                 │                 │               │───────────────>│
  │                 │                 │               │                │
  │                 │                 │               │ in-flight reqs │
  │                 │                 │               │ unaffected     │
```

## Header Coverage

| Step | Function | Header | Status |
|------|----------|--------|--------|
| Begin transaction | `cog_config_begin()` | `config.h` | ✅ |
| Set values | `cog_config_set()` | `config.h` | ✅ |
| Commit | `cog_config_commit()` | `config.h` | ✅ |
| Validators | `cog_config_register_validator()` | `config.h` | ✅ |
| Rollback on failure | `cog_config_rollback()` | `config.h` | ✅ |
| Get values | `cog_config_get()` | `config.h` | ✅ |

## Gaps Found

1. **Change notification** — after commit, how do subsystems know the config changed? There's no `cog_config_on_change()` callback or event. **Fix: add `COG_EVENT_CONFIG_CHANGED` event kind, or a `cog_config_watch()` function.**

2. **Provider registry rebuild** — the provider registry needs to rebuild its endpoint table from the new config. But there's no `cog_provider_registry_reload()` function. **Fix: add `cog_provider_registry_from_config()` or document that the application handles this.**

3. **In-flight request isolation** — requests already in flight use the old provider/endpoint. The contract should state: "In-flight provider instances are not affected by config changes. New requests use the new endpoint table."

## Verdict

Headers cover the flow. One amendment needed:
- Add `COG_EVENT_CONFIG_CHANGED` event kind
- Document that in-flight requests are unaffected by config changes
