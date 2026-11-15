# Scenario 13: Session persistence, recovery and v1→v2 migration

Stage 14.D pass-2 retrace from public-contract baseline `29b41e8`.
This replaces the earlier schematic SVG as the normative call trace. The SVG is
retained as historical design material and is not current API coverage.

## Sequence diagram

```mermaid
sequenceDiagram
    participant App
    participant Session
    participant Store
    App->>Session: cog_session_append(structured message)
    App->>Store: cog_store_save(session)
    Store->>Store: write immutable v2 generation; fsync; atomic manifest
    App->>Store: cog_store_load_ex(id,REPLAY,report)
    alt v1 text-only
      Store-->>App: migrated in memory; no rewrite until explicit save
    else v1 unidentified tools / unknown version
      Store-->>App: refuse replay; explicit INSPECT only for legacy tools
    else v2 STARTED ledger
      Store-->>App: INDETERMINATE; replay_safe false
    end
```

## Exact public-header coverage

Names below are exact declared functions, types, enum members or struct fields;
application-only policy/envelope steps are identified explicitly rather than
presented as library calls. Each header path names the declaring public file.

| Trace step | Exact declaration / member | Declaring header |
|---|---|---|
| Store construction / format | `cog_store_new`; `cog_store_config_t`; `COG_STORE_FORMAT_V2` | [libraries/cog-core/include/cog/session_store.h](../../libraries/cog-core/include/cog/session_store.h) |
| Atomic generation write | `cog_store_save`; `cog_store_append`; `cog_store_vtable_t.save`; `cog_store_vtable_t.append` | [libraries/cog-core/include/cog/session_store.h](../../libraries/cog-core/include/cog/session_store.h) |
| Explicit load policy / report | `cog_store_load_ex`; `COG_STORE_LOAD_REPLAY`; `COG_STORE_LOAD_INSPECT`; `cog_store_load_report_t` | [libraries/cog-core/include/cog/session_store.h](../../libraries/cog-core/include/cog/session_store.h) |
| Structured copied node | `cog_session_append`; `cog_session_add_message`; `cog_node_data_t.message` | [libraries/cog-core/include/cog/session.h](../../libraries/cog-core/include/cog/session.h) |
| Persisted text/tool/opaque/usage | `cog_message_t`; `cog_message_tool_call_t`; `cog_message_opaque_t`; `cog_usage_t` | [libraries/cog-core/include/cog/message.h](../../libraries/cog-core/include/cog/message.h) |
| Uncertain effect state | `COG_TOOL_EXECUTION_STARTED`; `COG_TOOL_EXECUTION_INDETERMINATE` | [libraries/cog-core/include/cog/session.h](../../libraries/cog-core/include/cog/session.h) |

## Ownership, errors and adversarial branches

- Existing storage is v1, not unversioned. Text-only v1 can migrate in memory; explicit save writes v2. Unidentified v1 tools cannot receive invented IDs or silently become replay-safe.
- v2 retains complete assistant/tool associations, exact opaque byte lengths/namespace/format, usage-known bits and branch-local execution ledger.
- Only opt-in recovery of one unterminated final record after a validated committed prefix is permitted. Middle corruption, bad complete records, unsupported versions/metadata and invalid associations fail.
- Save publishes one durable generation manifest after fsync; independently replacing node/index files is not atomic. IO around publication is uncertain and cannot admit effects.
- STARTED loading becomes INDETERMINATE, even if a crash may have occurred before the actual effect. Missing derived index may rebuild only from validated authoritative log.

## Gap analysis and release amendment

No public `cog_session_save/load/serialize/deserialize` or `file_save` function exists; the store owns those boundaries. Custom backends require the new load_ex operation and must reject unsupported format/semantic capabilities. Actual v2 codec/migration remains gated delivery G.

## Executable evidence and required follow-up

Existing baseline test: [`applications/cog-cli/tests/test_13_session_persistence.c`](../../applications/cog-cli/tests/test_13_session_persistence.c). It exists as historical
engine/mock application evidence. It was not rerun in this design-only pass and
does not prove the new mandatory headers, real transport or durable-tool behavior.

Future conformance requirements: v1 text migration/no implicit rewrite; unsafe v1 INSPECT; opaque exact roundtrip; crash at every STARTED/final save boundary; malformed middle/tail and manifest failure injection.

Header tracing is complete only where the gap analysis says so. Production
implementation and executable scenario migration remain gated by pass 3 and the
independent final review; no passing runtime result is inferred from these tables.
