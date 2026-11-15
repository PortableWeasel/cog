# Scenario 16: Provider error propagation to user and durable state

Stage 14.D pass-2 retrace from public-contract baseline `29b41e8`.
This replaces the earlier schematic SVG as the normative call trace. The SVG is
retained as historical design material and is not current API coverage.

## Sequence diagram

```mermaid
sequenceDiagram
    participant Provider
    participant App
    participant Obs
    participant UI
    participant Store
    Provider-->>App: retained FINISH(outcome)
    App->>Provider: cog_provider_get_error(snapshot)
    App->>Obs: cog_obs_log(sanitized classification fields)
    App->>UI: bounded sanitized error envelope
    App->>Store: save explicit failed/incomplete turn
    alt User requests retry and replay is safe
      App->>App: apply remaining application retry budget
    else Unsafe partial output / effects / abort
      App->>App: preserve failure; no transparent replay
    end
```

## Exact public-header coverage

Names below are exact declared functions, types, enum members or struct fields;
application-only policy/envelope steps are identified explicitly rather than
presented as library calls. Each header path names the declaring public file.

| Trace step | Exact declaration / member | Declaring header |
|---|---|---|
| Terminal/error snapshot | `cog_provider_next`; `cog_provider_get_error` | [libraries/cog-provider/include/cog/provider_vtable.h](../../libraries/cog-provider/include/cog/provider_vtable.h) |
| Error safety fields | `cog_provider_error_t.code`; `cog_provider_error_t.http_status`; `cog_provider_error_t.kind`; `cog_provider_error_t.message`; `cog_provider_error_t.output_accepted`; `cog_provider_error_t.replay_safe` | [libraries/cog-provider/include/cog/provider_request.h](../../libraries/cog-provider/include/cog/provider_request.h) |
| Application events | `COG_EVENT_PROVIDER_ERROR`; `COG_EVENT_ERROR`; `COG_EVENT_RETRY`; `COG_EVENT_ABORT` | [libraries/cog-core/include/cog/event.h](../../libraries/cog-core/include/cog/event.h) |
| Structured log | `cog_obs_log` | [libraries/cog-obs/include/cog/obs.h](../../libraries/cog-obs/include/cog/obs.h) |
| Severity/field types | `cog_obs_severity_t`; `cog_obs_field_kind_t`; `cog_obs_field_t`; `cog_obs_field_table_t` | [libraries/cog-obs/include/cog/obs_types.h](../../libraries/cog-obs/include/cog/obs_types.h) |
| UI transport | `cog_ipc_endpoint_send`; `cog_ipc_message_t` | [libraries/cog-ipc/include/cog/ipc.h](../../libraries/cog-ipc/include/cog/ipc.h) |
| Explicit message status | `COG_MESSAGE_FAILED`; `COG_MESSAGE_INCOMPLETE`; `cog_message_t.status` | [libraries/cog-core/include/cog/message.h](../../libraries/cog-core/include/cog/message.h) |
| Durable error state | `cog_store_save` | [libraries/cog-core/include/cog/session_store.h](../../libraries/cog-core/include/cog/session_store.h) |

## Ownership, errors and adversarial branches

- Provider error strings are sanitized stable metadata, not raw HTTP bodies/headers, credential-bearing URLs or private source text.
- One terminal item ends a request. A fatal core service-hook error propagates separately; normal request failures arrive as terminal outcomes rather than being silently swallowed.
- UI/log/store encoding must preserve classification and explicit incomplete status without secrets. Unknown usage must not be guessed.
- User retry/abort events do not bypass replay safety, unresolved tool intent or elapsed/attempt limits. A retry after partial output is an explicit continuation decision, not hidden resend.

## Gap analysis and release amendment

`cog_obs_log_error` and `cog_ipc_send` are nonexistent convenience names; this trace uses generic structured logging and endpoint sending. Application state names are application-defined static dispatch entries, not missing core enum values.

## Executable evidence and required follow-up

Existing baseline test: [`applications/cog-cli/tests/test_16_error_propagation.c`](../../applications/cog-cli/tests/test_16_error_propagation.c). It exists as historical
engine/mock application evidence. It was not rerun in this design-only pass and
does not prove the new mandatory headers, real transport or durable-tool behavior.

Future conformance requirements: Sanitization with credential-like error input; terminal/cancel race; partial-output failure persistence; infrastructure error propagation; no duplicate retry after accepted effects.

Header tracing is complete only where the gap analysis says so. Production
implementation and executable scenario migration remain gated by pass 3 and the
independent final review; no passing runtime result is inferred from these tables.
