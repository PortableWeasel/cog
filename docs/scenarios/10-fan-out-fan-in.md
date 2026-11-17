# Scenario 10: Fan-out/fan-in across auxiliary cores

O9 executable update: the canonical CTest runs one local and two distinct
authenticated native children, ordered fan-in, repeated reuse, cancellation and
admission refusal. Historical placement/registry assertions remain in
`component-fan_out_fan_in`. See the
joined evidence and exact limits (`tix show f63652b`).
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
    participant Children
    loop bounded task count and capacity
      Parent->>Core: cog_agent_spawn(config with group_id,id)
      Core-->>Parent: queued SUBAGENT_SPAWN intent
      Parent->>Core: copy_config, then application dispatch with chosen placement
    end
    Children-->>Parent: CGA1 encoded terminal result envelopes
    Parent->>Parent: cog_agent_update_decode into owned bounded storage
    Parent->>Core: cog_agent_apply_update(id,current peer/epoch/sequence)
    Parent->>Core: copy_update; enqueue owned payload; ack_update
    alt First accepted winner / group cancellation
      Parent->>Core: cog_agent_cancel(remaining ids)
      Parent->>Core: cog_agent_is_cancelled(id,cancelled)
    else All required outcomes collected
      Parent->>Parent: aggregate each child once
    end
    Note over Parent,Children: Application tracks bounded group member IDs; each attempt is fenced
```

## Exact public-header coverage

Names below are exact declared functions, types, enum members or struct fields;
application-only policy/envelope steps are identified explicitly rather than
presented as library calls. Each header path names the declaring public file.

| Trace step | Exact declaration / member | Declaring header |
|---|---|---|
| Group and child identity | `cog_agent_config_t.group_id`; `cog_agent_spawn`; `cog_agent_id_t` | [libraries/cog-core/include/cog/agent.h](../../libraries/cog-core/include/cog/agent.h) |
| Placement / capacity | `cog_orchestrator_place`; `cog_orchestrator_list`; `cog_core_info_t.capacity` | [libraries/cog-orchestrator/include/cog/orchestrator.h](../../libraries/cog-orchestrator/include/cog/orchestrator.h) |
| Result and cancellation query | `cog_agent_result`; `cog_agent_cancel`; `cog_agent_is_cancelled`; `cog_agent_status` | [libraries/cog-core/include/cog/agent.h](../../libraries/cog-core/include/cog/agent.h) |
| Constructor-bounded child registry | `cog_core_config_t.max_agents`; `cog_core_config_t.max_agent_message_bytes` | [libraries/cog-core/include/cog/core.h](../../libraries/cog-core/include/cog/core.h) |
| Spawn-bound owner and buffers | `cog_agent_config_t.owner_peer`; `cog_agent_config_t.execution_epoch`; `cog_agent_config_t.max_status_bytes`; `cog_agent_config_t.max_result_bytes` | [libraries/cog-core/include/cog/agent.h](../../libraries/cog-core/include/cog/agent.h) |
| Portable lifecycle envelope | `cog_agent_update_encode`; `cog_agent_update_decode` | [libraries/cog-core/include/cog/agent.h](../../libraries/cog-core/include/cog/agent.h) |
| Atomic attempt ingestion | `cog_agent_apply_update`; `cog_agent_update_t.owner_peer`; `cog_agent_update_t.execution_epoch`; `cog_agent_update_t.sequence`; `cog_agent_update_t.status`; `cog_agent_update_t.outcome`; `cog_agent_update_t.data`; `cog_agent_update_t.data_len` | [libraries/cog-core/include/cog/agent.h](../../libraries/cog-core/include/cog/agent.h) |
| Stable notification delivery / release | `cog_agent_copy_update`; `cog_agent_ack_update`; `cog_agent_release` | [libraries/cog-core/include/cog/agent.h](../../libraries/cog-core/include/cog/agent.h) |
| Routed envelopes | `cog_ipc_send_to_session`; `cog_ipc_broadcast` | [libraries/cog-ipc/include/cog/ipc.h](../../libraries/cog-ipc/include/cog/ipc.h) |
| Terminal notifications | `COG_EVENT_SUBAGENT_RESULT`; `COG_EVENT_SUBAGENT_TIMEOUT` | [libraries/cog-core/include/cog/event.h](../../libraries/cog-core/include/cog/event.h) |
| Fair dispatch | `cog_core_config_t.service_interval`; `cog_core_config_t.ipc_service_quantum` | [libraries/cog-core/include/cog/core.h](../../libraries/cog-core/include/cog/core.h) |
| Nonblocking owned-process lifecycle | `cog_orchestrator_service`; `cog_orchestrator_request_shutdown`; `cog_orchestrator_config_t.service_quantum`; `cog_orchestrator_config_t.max_sessions` | [libraries/cog-orchestrator/include/cog/orchestrator.h](../../libraries/cog-orchestrator/include/cog/orchestrator.h) |

## Ownership, errors and adversarial branches

- Orchestrator spawn/stop admit nonblocking intent; readiness, heartbeat, timeout and reap progress through `cog_orchestrator_service`. UNKNOWN peers receive no session work. The legacy blocking shutdown helper is excluded from responsive hooks; use request_shutdown/service and free only after all owned children are reaped. Auto-replacement creates idle capacity, never restarts an unresolved task.

- Set finite fan-out, runtime and output bounds. Partial admission failure does not silently drop already-spawned children; track and cancel/drain each admitted ID.
- Aggregation accepts one terminal outcome per child attempt. A late result after cancellation cannot become a second winning result or authorize a tool.
- `group_id` groups calls but is not a built-in `fan_out` or `fan_in` operation; the application tracks its explicit bounded member set.
- Broadcast is a transport operation, not a reliable group cancellation completion acknowledgement. Continue servicing until admitted children reach terminal/indeterminate states.

## Gap analysis and release amendment

**O10-G1 resolved by pass-2 typed attempt ingress and retained notification:** a bounded application member-ID set plus existing group_id suffices for fan-out/fan-in; every child completion goes through public apply/copy/ack. Terminal duplicates are idempotent only when identical and cannot produce another aggregate contribution. No special fan-out API or private transition hook is required. Implementation must still prove public multi-core conformance.

## Executable evidence and required follow-up

Existing baseline test: [`tests/scenarios/test_10_fan_out_fan_in.c`](../../tests/scenarios/test_10_fan_out_fan_in.c). It exists as historical
engine/mock application evidence. It was not rerun in this design-only pass and
does not prove the new mandatory headers, real transport or durable-tool behavior.

Future conformance requirements: Partial spawn failure cleanup, winner/late result race, busy queue fairness, bounded aggregation and no duplicate child/tool effects.

Header tracing is complete only where the gap analysis says so. Production
implementation and executable scenario migration remain gated by pass 3 and the
independent final review; no passing runtime result is inferred from these tables.

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
