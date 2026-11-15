# R10: Save, restart, version migration and safe recovery

Stage 14.D pass 2; baseline header pass 1 `29b41e8`, amended by this pass.
This is a contract trace, **not an executable conformance pass**.

## Sequence

```mermaid
sequenceDiagram
    CLI->>Store: cog_store_save(store,session)
    Note over Store: fsync complete generation -> atomic manifest -> directory fsync
    CLI->>Store: cog_store_load_ex(store,id,REPLAY,session,report)
    alt v1 text-only
    Store-->>CLI: migrated_in_memory=true; source_version=1; no disk write
    else v1 unidentified tool history
    Store-->>CLI: UNSUPPORTED (INSPECT only, replay_safe=false)
    else v2 outstanding STARTED
    Store-->>CLI: INDETERMINATE; ordinary history refuses replay
    end
    CLI->>Session: cog_session_copy_history(session,node,msgs,capacity,count)
```

## Exact public coverage

Each symbol below is declared in the linked public header. Local application
decisions in the diagram are not invented library functions.

| Step | Exact calls, types, events or fields | Declaring header |
|---|---|---|
| Version-aware operations | `cog_store_delete`, `cog_store_delete_fn`, `cog_store_free`, `COG_STORE_FORMAT_V2`, `cog_store_save`, `cog_store_load_ex`, `cog_store_load_report_t`, `COG_STORE_LOAD_REPLAY`, `COG_STORE_LOAD_INSPECT` | [libraries/cog-core/include/cog/session_store.h](../../libraries/cog-core/include/cog/session_store.h) |
| Recovery policy | `cog_store_config_t`, `cog_store_append` | [libraries/cog-core/include/cog/session_store.h](../../libraries/cog-core/include/cog/session_store.h) |
| History refusal/ledger | `cog_session_copy_history`, `cog_session_tool_state`, `COG_TOOL_EXECUTION_INDETERMINATE` | [libraries/cog-core/include/cog/session.h](../../libraries/cog-core/include/cog/session.h) |
| Exact structured content | `cog_message_t`, `cog_message_opaque_t`, `cog_usage_t` | [libraries/cog-core/include/cog/message.h](../../libraries/cog-core/include/cog/message.h) |


## Ownership and invariants

- v1 already exists; this is v1->v2 migration, not initial versioning. Text-only migration is lossless in memory; explicit save publishes v2. Never invent call IDs/effects for v1 tool records.
- v2 atomically commits tree identity, associated results and ledger, usage availability and exact opaque bytes. No independent node/index publication. Unknown versions/unsupported custom backend fail UNSUPPORTED.
- Tail recovery may discard only an unterminated record AFTER the manifest-validated committed prefix. Missing committed bytes/results/STARTED, middle corruption or malformed complete records fail. It must not erase an effect by calling it a tail.
- Failed directory durability sync means IO/uncertain, never permission to execute after STARTED. Crash after a real effect but before final result publication loads INDETERMINATE without rerun.

## Failure and capacity branches

- Owned-session or ownership-anchor deletion returns UNSUPPORTED through both public and backend paths; require_owner=false is not a bypass. Race deletion against bootstrap/adoption and verify the stable anchor is never unlinked/recreated. Free the store only after all guards/borrowers are released.

- Fixtures must include v1 text, v1 tools inspect/refusal, v2 complete, unknown version, STARTED, unknown opaque namespace, missing derived index and each atomic-publish crash point.
- INSPECT is not a bypass for provider_send or tools. It allows observation only; replay_safe=false is propagated.

## Evidence and gap disposition

Original13/17 baseline tests and current session_store.c use v1; they do not prove v2 migration/recovery.

**Required future executable evidence:** G/L: byte-exact opaque fixture save/load/fork; generation crash fault injection; malicious association and committed-prefix truncation tests.

**Amendment references:** D07,D08 in [pass-2 register](../REAL_CLIENT_SCENARIO_PASS2.md).
