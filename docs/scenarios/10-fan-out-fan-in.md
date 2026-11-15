# Scenario 10: Fan-out/fan-in across auxiliary cores

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
      Core->>Orchestrator: placement for each admitted child
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
