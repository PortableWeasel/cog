# Scenario 07: Auxiliary core crash → detection → safe recovery

Stage 14.D pass-2 retrace from public-contract baseline `29b41e8`.
This replaces the earlier schematic SVG as the normative call trace. The SVG is
retained as historical design material and is not current API coverage.

## Sequence diagram

```mermaid
sequenceDiagram
    participant Parent
    participant Core
    participant Orchestrator
    participant IPC
    participant Store
    Orchestrator->>Orchestrator: heartbeat/health check
    IPC-->>Parent: COG_EVENT_IPC_DISCONNECTED
    Parent->>Orchestrator: service/reap and cog_orchestrator_sessions(dead_peer)
    Parent->>Parent: fence/reap failed attempt before synthesizing failure
    Parent->>Core: cog_agent_apply_update(FAILED with INDETERMINATE outcome)
    Parent->>Store: cog_store_load_ex(REPLAY,report)
    Store-->>Parent: STARTED becomes INDETERMINATE
    alt Existing attempt may have effects
      Parent->>Parent: report failure; no automatic child/tool replay
    else Safe new placement explicitly admitted
      Parent->>Orchestrator: cog_orchestrator_spawn(replacement core)
    end
    Note over Parent,Orchestrator: Core replacement is distinct from replaying a child task
```

## Exact public-header coverage

Names below are exact declared functions, types, enum members or struct fields;
application-only policy/envelope steps are identified explicitly rather than
presented as library calls. Each header path names the declaring public file.

| Trace step | Exact declaration / member | Declaring header |
|---|---|---|
| Health / affected sessions | `cog_orchestrator_health`; `cog_orchestrator_heartbeat`; `cog_orchestrator_sessions`; `COG_HEALTH_DEAD` | [libraries/cog-orchestrator/include/cog/orchestrator.h](../../libraries/cog-orchestrator/include/cog/orchestrator.h) |
| Replacement process | `cog_orchestrator_config_t.auto_replace`; `cog_orchestrator_spawn` | [libraries/cog-orchestrator/include/cog/orchestrator.h](../../libraries/cog-orchestrator/include/cog/orchestrator.h) |
| Disconnect notification | `COG_EVENT_IPC_DISCONNECTED`; `cog_event_ipc_t` | [libraries/cog-core/include/cog/event.h](../../libraries/cog-core/include/cog/event.h) |
| Portable lifecycle envelope | `cog_agent_update_encode`; `cog_agent_update_decode` | [libraries/cog-core/include/cog/agent.h](../../libraries/cog-core/include/cog/agent.h) |
| Atomic attempt ingestion | `cog_agent_apply_update`; `cog_agent_update_t.owner_peer`; `cog_agent_update_t.execution_epoch`; `cog_agent_update_t.sequence`; `cog_agent_update_t.status`; `cog_agent_update_t.outcome`; `cog_agent_update_t.data`; `cog_agent_update_t.data_len` | [libraries/cog-core/include/cog/agent.h](../../libraries/cog-core/include/cog/agent.h) |
| Stable notification delivery / release | `cog_agent_copy_update`; `cog_agent_ack_update`; `cog_agent_release` | [libraries/cog-core/include/cog/agent.h](../../libraries/cog-core/include/cog/agent.h) |
| Read-only child state | `cog_agent_status`; `cog_agent_is_cancelled`; `cog_agent_status_t` | [libraries/cog-core/include/cog/agent.h](../../libraries/cog-core/include/cog/agent.h) |
| Durable crash report | `cog_store_load_ex`; `cog_store_load_report_t.indeterminate_tools`; `cog_store_load_report_t.replay_safe` | [libraries/cog-core/include/cog/session_store.h](../../libraries/cog-core/include/cog/session_store.h) |
| No effect replay | `COG_TOOL_EXECUTION_STARTED`; `COG_TOOL_EXECUTION_INDETERMINATE`; `cog_session_tool_state` | [libraries/cog-core/include/cog/session.h](../../libraries/cog-core/include/cog/session.h) |
| Nonblocking owned-process lifecycle | `cog_orchestrator_service`; `cog_orchestrator_request_shutdown`; `cog_orchestrator_config_t.service_quantum`; `cog_orchestrator_config_t.max_sessions` | [libraries/cog-orchestrator/include/cog/orchestrator.h](../../libraries/cog-orchestrator/include/cog/orchestrator.h) |

## Ownership, errors and adversarial branches

- Orchestrator spawn/stop admit nonblocking intent; readiness, heartbeat, timeout and reap progress through `cog_orchestrator_service`. UNKNOWN peers receive no session work. The legacy blocking shutdown helper is excluded from responsive hooks; use request_shutdown/service and free only after all owned children are reaped. Auto-replacement creates idle capacity, never restarts an unresolved task.

- An owned auxiliary process may be replaced after crash; that is not authorization to replay its task or tool effects.
- STARTED means the side effect may or may not have occurred. Normal load converts it to INDETERMINATE and ordinary replay is refused.
- Losing a peer invalidates current routing; stale status/results from an earlier attempt cannot mutate a replacement attempt.
- `cog_agent_status` is a query, not a "mark failed" setter. Preserve the distinction between observed process health and durable child execution outcome.

## Gap analysis and release amendment

**O07-G1 resolved by pass-2 attempt ingress:** after fencing/reaping the failed attempt, the parent may synthesize a FAILED/TIMEOUT update with the bound peer/epoch/sequence and uncertain-effects outcome. apply/copy/ack preserves one immutable terminal notification and rejects stale child messages. Core replacement is distinct from effect replay. No remaining contract gap is claimed; public orchestrator service drives process cleanup.

## Executable evidence and required follow-up

Existing baseline test: [`tests/scenarios/test_07_aux_crash.c`](../../tests/scenarios/test_07_aux_crash.c). It exists as historical
engine/mock application evidence. It was not rerun in this design-only pass and
does not prove the new mandatory headers, real transport or durable-tool behavior.

Future conformance requirements: Crash before/after durable STARTED, before/after effect, stale late results, safe replacement without task replay; public-only lifecycle ingestion.

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
