# Scenario 03: Subagent spawn → execute → status → result → parent

O9 executable update: the canonical CTest now runs actual local/native REAL
child work, durable parent results and cancellation. Historical registry-only
assertions remain in `component-agent_lifecycle`. See the
[joined evidence and exact limits](../REAL_CLIENT_O9_SCENARIO_EVIDENCE.md).
The contract/design-pass history below is retained as such.

Stage 14.D pass-2 retrace from public-contract baseline `29b41e8`.
This replaces the earlier schematic SVG as the normative call trace. The SVG is
retained as historical design material and is not current API coverage.

## Sequence diagram

```mermaid
sequenceDiagram
    participant Parent
    participant Core
    participant Orchestrator
    participant Child
    Parent->>Orchestrator: cog_orchestrator_place(load)
    Parent->>Core: cog_agent_spawn(config with owner_peer and execution_epoch,id)
    Core-->>Parent: queued SUBAGENT_SPAWN leased intent
    Parent->>Core: cog_agent_copy_config into owned task/parent/epoch storage
    Parent->>Child: application dispatch or framed remote task envelope
    Child->>Child: cog_agent_update_encode(CGA1)
    Child->>Core: routed CGA1 status/result envelope
    Parent->>Parent: cog_agent_update_decode into owned storage; validate peer
    Parent->>Core: cog_agent_apply_update(id,update)
    Parent->>Core: cog_agent_copy_update(id,owned event storage)
    Parent->>Core: enqueue copied notification; ack_update only after success
    Parent->>Core: cog_agent_status(id,status)
    Parent->>Core: cog_agent_result(id,buffer,capacity,length)
    alt Parent cancels or child times out
      Parent->>Core: cog_agent_cancel(id)
      Core->>Child: cancellation request; continue fair servicing
    end
```

## Exact public-header coverage

Names below are exact declared functions, types, enum members or struct fields;
application-only policy/envelope steps are identified explicitly rather than
presented as library calls. Each header path names the declaring public file.

| Trace step | Exact declaration / member | Declaring header |
|---|---|---|
| Spawn identity / bounds | `cog_agent_spawn`; `cog_agent_config_t.timeout_ms`; `cog_agent_config_t.force_local`; `cog_agent_config_t.group_id`; `cog_agent_id_t` | [libraries/cog-core/include/cog/agent.h](../../libraries/cog-core/include/cog/agent.h) |
| Placement / process ownership | `cog_orchestrator_place`; `cog_orchestrator_spawn`; `cog_orchestrator_sessions` | [libraries/cog-orchestrator/include/cog/orchestrator.h](../../libraries/cog-orchestrator/include/cog/orchestrator.h) |
| Session hierarchy | `cog_session_set_parent`; `cog_session_parent_id` | [libraries/cog-core/include/cog/session.h](../../libraries/cog-core/include/cog/session.h) |
| Routed envelope | `cog_ipc_message_t`; `cog_ipc_register_session`; `cog_ipc_send_to_session` | [libraries/cog-ipc/include/cog/ipc.h](../../libraries/cog-ipc/include/cog/ipc.h) |
| Status/result notifications | `COG_EVENT_SUBAGENT_STATUS`; `COG_EVENT_SUBAGENT_RESULT`; `COG_EVENT_SUBAGENT_TIMEOUT`; `cog_event_ipc_t` | [libraries/cog-core/include/cog/event.h](../../libraries/cog-core/include/cog/event.h) |
| Constructor-bounded child registry | `cog_core_config_t.max_agents`; `cog_core_config_t.max_agent_message_bytes` | [libraries/cog-core/include/cog/core.h](../../libraries/cog-core/include/cog/core.h) |
| Spawn-bound owner and buffers | `cog_agent_config_t.owner_peer`; `cog_agent_config_t.execution_epoch`; `cog_agent_config_t.max_status_bytes`; `cog_agent_config_t.max_result_bytes` | [libraries/cog-core/include/cog/agent.h](../../libraries/cog-core/include/cog/agent.h) |
| Portable lifecycle envelope | `cog_agent_update_encode`; `cog_agent_update_decode` | [libraries/cog-core/include/cog/agent.h](../../libraries/cog-core/include/cog/agent.h) |
| Atomic attempt ingestion | `cog_agent_apply_update`; `cog_agent_update_t.owner_peer`; `cog_agent_update_t.execution_epoch`; `cog_agent_update_t.sequence`; `cog_agent_update_t.status`; `cog_agent_update_t.outcome`; `cog_agent_update_t.data`; `cog_agent_update_t.data_len` | [libraries/cog-core/include/cog/agent.h](../../libraries/cog-core/include/cog/agent.h) |
| Stable notification delivery / release | `cog_agent_copy_update`; `cog_agent_ack_update`; `cog_agent_release` | [libraries/cog-core/include/cog/agent.h](../../libraries/cog-core/include/cog/agent.h) |
| Public read/cancel surface | `cog_agent_notify_parent`; `cog_agent_status`; `cog_agent_result`; `cog_agent_cancel` | [libraries/cog-core/include/cog/agent.h](../../libraries/cog-core/include/cog/agent.h) |
| Bounded nonblocking frame service | `cog_ipc_endpoint_new_ex`; `cog_ipc_endpoint_config_t.max_frame_bytes`; `cog_ipc_endpoint_config_t.receive_frames`; `cog_ipc_endpoint_config_t.send_frames`; `cog_ipc_endpoint_config_t.service_quantum`; `cog_ipc_endpoint_service` | [libraries/cog-ipc/include/cog/ipc.h](../../libraries/cog-ipc/include/cog/ipc.h) |
| Nonblocking owned-process lifecycle | `cog_orchestrator_service`; `cog_orchestrator_request_shutdown`; `cog_orchestrator_config_t.service_quantum`; `cog_orchestrator_config_t.max_sessions` | [libraries/cog-orchestrator/include/cog/orchestrator.h](../../libraries/cog-orchestrator/include/cog/orchestrator.h) |

## Ownership, errors and adversarial branches

- Orchestrator spawn/stop admit nonblocking intent; readiness, heartbeat, timeout and reap progress through `cog_orchestrator_service`. UNKNOWN peers receive no session work. The legacy blocking shutdown helper is excluded from responsive hooks; use request_shutdown/service and free only after all owned children are reaped. Auto-replacement creates idle capacity, never restarts an unresolved task.

- Every endpoint has constructor-reserved frame queues and bounded nonblocking service. A peer that sends a partial length prefix or stalls halfway through payload retains its own cursor and cannot block unrelated peers. Send OK copies one frame exactly once; retry only AGAIN, which accepted nothing. Receive OVERFLOW reports required bytes and retains the same complete frame across service calls.
- Placement failure returns UNAVAILABLE before a child is admitted; spawn/OOM must leave no orphan child session or process.
- Spawn reserves task/status/result storage, selected owner peer and nonreused attempt epoch. Ingress checks matching peer/epoch and contiguous sequence; identical last update is idempotent, conflicting duplicates or stale/late terminal data fail without changing the record.
- CGA1 uses fixed-width big-endian IDs/peer/epoch/sequence/status/outcome/length plus exact UTF-8 bytes. Native structs/pointers are never transmitted; version/status/outcome/UTF-8/length/trailing-byte failures publish nothing.
- Only one unacknowledged notification is retained. A distinct update gets AGAIN until the prior snapshot is copied into independently owned event storage, successfully enqueued and acknowledged. Queue/copy overflow consumes neither upstream update nor notification; later ingestion cannot invalidate queued event bytes.
- `cog_agent_status` only queries. A result envelope is not permission for an application to mutate a private record.
- Cancellation is cooperative and late results must be correlated to the still-current child attempt. A child with unresolved tool intent is not restarted automatically.

## Gap analysis and release amendment

**O03-G1 resolved by pass-2 public agent update contract:** apply/copy/ack plus explicit owner peer, nonreused epoch, sequence and constructor capacities replace private-record mutation and unsafe borrowed notification lifetime. Terminal release is explicit and refuses active/unacknowledged records. No unresolved contract gap remains in this lifecycle trace; Stage 14.N/O implement the public protocol rather than invent it.

## Executable evidence and required follow-up

Existing baseline test: [`tests/scenarios/test_03_subagent_lifecycle.c`](../../tests/scenarios/test_03_subagent_lifecycle.c). It exists as historical
engine/mock application evidence. It was not rerun in this design-only pass and
does not prove the new mandatory headers, real transport or durable-tool behavior.

Future conformance requirements: Public-only child runtime; owned queued status storage; duplicate/late result; cancel/timeout race; no private agent record hooks.

Header tracing is complete only where the gap analysis says so. Production
implementation and executable scenario migration remain gated by pass 3 and the
independent final review; no passing runtime result is inferred from these tables.

## Pass-3 IPC ownership retrace (E02)

Core constructor settings `cog_core_config_t.ipc_max_frame_bytes` and
`cog_core_config_t.ipc_payload_bytes` bound admitted frames and queued payload
bytes ([core.h](../../libraries/cog-core/include/cog/core.h)). Reserve an event
slot and owned arena bytes before endpoint receive; if unavailable, consume
nothing and service/dispatch other work. Bytes survive until handler return.
An endpoint-retained frame above the core ceiling closes only that peer with
reported LIMIT, rather than an endless OVERFLOW loop. Accepted endpoint
constructors remain explicit and peer-count-bounded; private intrusive listener
links need no additional public allocation surface. Test just-below/exact/above
frame limits, full arena/full event queue, and caller payload mutation after enqueue.

## Local-agent intent amendment (86f7a3c under bf7a450)

The core queues intent; it does not invoke an application runtime or know a
remote wire protocol. The static application dispatcher consumes these events.
This is a targeted header gap amendment after E, not a new release-gate claim.

```mermaid
sequenceDiagram
    App->>Core: cog_agent_spawn(config,id)
    Core->>Core: reserve record/child/payload and atomically queue SPAWN
    Core-->>App: SUBAGENT_SPAWN typed payload under dispatch lease
    App->>Core: cog_agent_copy_config(id,config,owned bytes,capacity,required)
    App->>Core: cog_agent_is_cancelled(id,cancelled) before effects
    alt local and not cancelled
      App->>Runtime: start copied task with actual nonzero local epoch
    else remote and not cancelled
      App->>Wire: forward copied correlated bounded application protocol
    end
    App->>Core: cog_agent_cancel(id)
    Core-->>App: SUBAGENT_CANCEL leased identity/action
    App->>Runtime: cancel or forward; drain/reap before terminal update
    App->>Core: apply_update; copy_update; enqueue notification; ack_update
    App->>Core: release only after terminal, notification ack and intent leases end
```

| Intent step | Exact public declaration | Declaring header |
|---|---|---|
| Static dispatch convention and leased payload | `COG_EVENT_SUBAGENT_SPAWN`; `COG_EVENT_SUBAGENT_CANCEL`; `cog_event_agent_intent_t.agent_id`; `cog_event_agent_intent_t.execution_epoch`; `cog_event_agent_intent_t.owner_peer`; `cog_event_agent_intent_t.action` | [event.h](../../libraries/cog-core/include/cog/event.h) |
| Copy before deferred work / first update | `cog_agent_copy_config`; `cog_agent_config_t.task`; `cog_agent_config_t.parent_session_id`; `cog_agent_config_t.execution_epoch` | [agent.h](../../libraries/cog-core/include/cog/agent.h) |
| Atomic admission and release lifetime | `cog_agent_spawn`; `cog_agent_cancel`; `cog_agent_is_cancelled`; `cog_agent_release` | [agent.h](../../libraries/cog-core/include/cog/agent.h) |
| Origin and lease unwind | `cog_core_enqueue`; `cog_core_run`; `cog_core_free` | [core.h](../../libraries/cog-core/include/cog/core.h) |

1. **Header pass 1:** append event kinds without renumbering earlier values.
   Core-originated event ID is `(cog_event_id_t)kind`, source_id is agent ID;
   static application rows handle those IDs, which are not globally reserved.
   A pointer-free typed payload carries action and exact ID/epoch/placement.
2. **Targeted trace pass 2:** local epoch=0 resolves to assigned agent ID, so
   copy_config or the spawn event supplies it before the first update. Copy
   OVERFLOW changes required bytes only; successful task/optional-parent bytes
   remain caller-owned after dispatch/release. A remote handler forwards its
   own protocol; arbitrary IPC delivery is not an agent execution grant.
3. **Consistency pass 3:** queue-full spawn leaves no child/record/event/lease
   or output ID; first cancel OVERFLOW changes no state. Repeated admitted
   active cancel is OK without another event, terminal cancel is INVALID_STATE.
   If cancel is admitted before spawn dispatch, the handler checks cancelled
   and starts no effects. Terminal can be applied early, but release still
   refuses queued/in-dispatch intent leases. Lease unwind occurs after every
   dispatch result, including UNHANDLED/handler/transition error. Deferred work
   must copy scalars and strings; destruction never executes queued intents.

The core reserves one spawn and one cancel payload per bounded agent record;
no per-intent allocation, dynamic registry or provider/orchestrator link is added.
Public core_enqueue rejects these origin-controlled kinds with INVALID_ARG,
preventing an unleased forged duplicate. Generic event queues still borrow the
payload pointer, so this does not require a new deep-copy queue policy.

Required bf7a450 executable checks: exact dispatch IDs/action/owner/assigned epoch;
caller task mutation after spawn; config capacity retry before first update; full
queue spawn/cancel rollback; duplicate cancellation; cancel-before-spawn; terminal
release blocked until pending spawn/cancel handlers return (including errors);
owned deferred config after release; no implicit remote wire send. This design
commit supplies declarations/traces only; writer and independent code review
must establish those runtime results before package completion.
