# Scenario 05: Graceful shutdown with in-flight work

Stage 14.D pass-2 retrace from public-contract baseline `29b41e8`.
This replaces the earlier schematic SVG as the normative call trace. The SVG is
retained as historical design material and is not current API coverage.

## Sequence diagram

```mermaid
sequenceDiagram
    participant Signal
    participant App
    participant Core
    participant Provider
    participant Tool
    participant Host
    Signal->>Core: cog_core_stop() only signal-safe entry
    Core-->>App: run returns; async ownership remains
    App->>Provider: cog_provider_cancel()
    App->>Tool: cog_toolkit_execution_cancel(now)
    loop bounded cleanup until tool done
      App->>Tool: cog_toolkit_execution_poll(now,done,deadline)
    end
    App->>App: save incomplete/cancelled turn and final tool ledger
    App->>Tool: cog_toolkit_execution_free()
    App->>Core: cog_core_free()
    App->>Provider: cog_provider_disconnect()
    App->>Host: release net then host dependency cleanup
```

## Exact public-header coverage

Names below are exact declared functions, types, enum members or struct fields;
application-only policy/envelope steps are identified explicitly rather than
presented as library calls. Each header path names the declaring public file.

| Trace step | Exact declaration / member | Declaring header |
|---|---|---|
| Signal / loop exit | `cog_core_stop`; `cog_core_run`; `cog_core_free` | [libraries/cog-core/include/cog/core.h](../../libraries/cog-core/include/cog/core.h) |
| Owner-thread shutdown request | `cog_cli_app_shutdown`; `cog_cli_app_stop` | [applications/cog-cli/include/cog_cli/cli.h](../../applications/cog-cli/include/cog_cli/cli.h) |
| Provider detachment / retained terminal | `cog_provider_cancel`; `cog_provider_next`; `cog_provider_disconnect` | [libraries/cog-provider/include/cog/provider_vtable.h](../../libraries/cog-provider/include/cog/provider_vtable.h) |
| Process group cleanup | `cog_toolkit_execution_cancel`; `cog_toolkit_execution_poll`; `cog_toolkit_execution_free` | [libraries/cog-toolkit/include/cog/toolkit_exec.h](../../libraries/cog-toolkit/include/cog/toolkit_exec.h) |
| Cancel state / ledger | `COG_MESSAGE_CANCELLED`; `COG_MESSAGE_INCOMPLETE` | [libraries/cog-core/include/cog/message.h](../../libraries/cog-core/include/cog/message.h) |
| Tool ledger transition | `cog_session_tool_transition`; `COG_TOOL_EXECUTION_CANCELLED`; `COG_TOOL_EXECUTION_INDETERMINATE` | [libraries/cog-core/include/cog/session.h](../../libraries/cog-core/include/cog/session.h) |
| Durability | `cog_store_save` | [libraries/cog-core/include/cog/session_store.h](../../libraries/cog-core/include/cog/session_store.h) |
| Owned auxiliary processes | `cog_orchestrator_shutdown` | [libraries/cog-orchestrator/include/cog/orchestrator.h](../../libraries/cog-orchestrator/include/cog/orchestrator.h) |
| Request/context destruction | `cog_net_request_release`; `cog_net_free` | [libraries/cog-net/include/cog/net.h](../../libraries/cog-net/include/cog/net.h) |
| Nonblocking owned-process lifecycle | `cog_orchestrator_service`; `cog_orchestrator_request_shutdown`; `cog_orchestrator_config_t.service_quantum`; `cog_orchestrator_config_t.max_sessions` | [libraries/cog-orchestrator/include/cog/orchestrator.h](../../libraries/cog-orchestrator/include/cog/orchestrator.h) |

## Ownership, errors and adversarial branches

- Orchestrator spawn/stop admit nonblocking intent; readiness, heartbeat, timeout and reap progress through `cog_orchestrator_service`. UNKNOWN peers receive no session work. The legacy blocking shutdown helper is excluded from responsive hooks; use request_shutdown/service and free only after all owned children are reaped. Auto-replacement creates idle capacity, never restarts an unresolved task.

- A signal handler must only request stop through the documented signal-safe entry; provider/tool/store/IPC calls occur on the owner thread afterward.
- Loop exit does not free application-owned asynchronous work. Cancel and drain it explicitly; a tool execution cannot be freed while active/cancelling. Free terminal executions before core_free because they borrow the core through execution_free; core then releases its owned IPC before caller-owned provider/obs/store/session teardown.
- Process tools TERM then deadline-driven KILL their owned group and reap children before done. No thread abandonment or resolver join workaround is permitted.
- A saved STARTED effect can become indeterminate if cleanup/durability is uncertain; shutdown never labels an unconfirmed effect as safely retryable.
- Retained provider/net bytes are consumed or explicitly discarded by teardown. Host dependency cleanup is last, after all Cog and non-Cog users.

## Gap analysis and release amendment

The API expresses cooperative shutdown and explicit cleanup; it does not guarantee arbitrary callback/OS wall time. A final shutdown completion is only valid after owned child cleanup and persistence policy finish, not merely after core_stop returns.

## Executable evidence and required follow-up

Existing baseline test: [`tests/scenarios/test_05_graceful_shutdown.c`](../../tests/scenarios/test_05_graceful_shutdown.c). It exists as historical
engine/mock application evidence. It was not rerun in this design-only pass and
does not prove the new mandatory headers, real transport or durable-tool behavior.

Future conformance requirements: Cancel during DNS/stream/backoff/tool; process-group reap; failed final save; stop under busy queue. Additional baseline regression: tests/scenarios/test_05b_shutdown_cancel_contract.c.

Header tracing is complete only where the gap analysis says so. Production
implementation and executable scenario migration remain gated by pass 3 and the
independent final review; no passing runtime result is inferred from these tables.
