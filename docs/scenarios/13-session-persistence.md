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

## Pass-3 recovery retrace (E01)

A crash leaves STARTED as INDETERMINATE. Ordinary guard acquisition still refuses
execution. After fencing/reaping the old incarnation and admitting a fresh epoch
against the exact latest checkpoint, an explicitly authorized operator uses the
restricted recovery path below; there is no automatic task or tool replay.

```mermaid
sequenceDiagram
    App->>Store: cog_store_owner_acquire_recovery(id,current_owner,guard)
    App->>Session: cog_session_reconcile_tool(assistant,id,resolution,identified_result)
    App->>Store: cog_store_save(reconciliation only)
    App->>Store: cog_store_owner_release(recovery_guard)
    App->>Store: owner_read; owner_compare_exchange(NEW epoch,exact checkpoint)
    App->>Store: cog_store_owner_acquire(normal guard) only after safe validation
```

| Recovery step | Exact declaration | Declaring header |
|---|---|---|
| Restricted ownership | `cog_store_owner_acquire_recovery`; `cog_store_owner_guard_borrow`; `cog_store_save` | [session_store.h](../../libraries/cog-core/include/cog/session_store.h) |
| Atomic explicit result closure | `cog_session_reconcile_tool`; `COG_TOOL_EXECUTION_ACKNOWLEDGED` | [session.h](../../libraries/cog-core/include/cog/session.h) |

Recovery guard cannot admit model/tool/child work or ordinary append/checkpoint.
Only explicit monotonic ledger/result reconciliation is saveable; all original
messages, opaque bytes and audit evidence remain. ACKNOWLEDGED preserves unknown
effects in its result instead of claiming they did not happen. A failed save or
crash retains uncertainty; it never repeats execution. Releasing recovery guard
does not itself grant execution: a new durable epoch and normal guard are required.
Future G/L tests must cover rejected execution borrows, wrong call IDs, capacity
failure atomicity, failed final save and crash after every recovery boundary.
