# Scenario 18: Structured observability and trace collection

Stage 14.D pass-2 retrace from public-contract baseline `29b41e8`.
This replaces the earlier schematic SVG as the normative call trace. The SVG is
retained as historical design material and is not current API coverage.

## Sequence diagram

```mermaid
sequenceDiagram
    participant App
    participant Obs
    participant Sink
    App->>Obs: cog_obs_trace_begin(span,id)
    App->>Obs: cog_obs_log(severity,message,typed fields)
    App->>Obs: cog_obs_diag_publish(snapshot)
    Obs->>Sink: configured borrowed sink callbacks
    App->>Obs: cog_obs_trace_end(id,status)
    Obs->>Sink: cog_obs_span_record_t
    alt Destination buffer too short
      App->>Obs: cog_obs_log_to / cog_obs_diag_format
      Obs-->>App: OVERFLOW with required length; no partial success
    end
```

## Exact public-header coverage

Names below are exact declared functions, types, enum members or struct fields;
application-only policy/envelope steps are identified explicitly rather than
presented as library calls. Each header path names the declaring public file.

| Trace step | Exact declaration / member | Declaring header |
|---|---|---|
| Explicit context and sinks | `cog_obs_new`; `cog_obs_config_t`; `cog_obs_sink_table_t`; `cog_obs_trace_sink_fn` | [libraries/cog-obs/include/cog/obs.h](../../libraries/cog-obs/include/cog/obs.h) |
| Typed structured fields | `cog_obs_field_desc_t`; `cog_obs_field_table_t`; `cog_obs_field_t`; `cog_obs_severity_t` | [libraries/cog-obs/include/cog/obs_types.h](../../libraries/cog-obs/include/cog/obs_types.h) |
| Generic logging and formatted copy | `cog_obs_log`; `cog_obs_log_to` | [libraries/cog-obs/include/cog/obs.h](../../libraries/cog-obs/include/cog/obs.h) |
| Trace nesting | `cog_obs_trace_begin`; `cog_obs_trace_end`; `cog_obs_trace_current`; `cog_obs_trace_depth` | [libraries/cog-obs/include/cog/obs.h](../../libraries/cog-obs/include/cog/obs.h) |
| Trace output record | `cog_obs_span_record_t`; `cog_obs_span_id_t` | [libraries/cog-obs/include/cog/obs_types.h](../../libraries/cog-obs/include/cog/obs_types.h) |
| Diagnostic snapshots | `cog_obs_diag_publish`; `cog_obs_diag_at`; `cog_obs_diag_count`; `cog_obs_diag_format`; `cog_obs_conn_snapshot_t` | [libraries/cog-obs/include/cog/obs.h](../../libraries/cog-obs/include/cog/obs.h) |
| Reported usage / metrics source | `cog_usage_t.tokens_known`; `cog_usage_t.cost_known` | [libraries/cog-core/include/cog/message.h](../../libraries/cog-core/include/cog/message.h) |
| Session timing/cost | `cog_session_set_timing`; `cog_session_set_cost`; `cog_session_cumulative_timing`; `cog_session_cumulative_cost` | [libraries/cog-core/include/cog/session.h](../../libraries/cog-core/include/cog/session.h) |

## Ownership, errors and adversarial branches

- Field descriptors and sink tables are static read-only data. Each obs context owns its scratch/span/diagnostic capacities; there is no global/thread-local ambient trace stack.
- Span end follows reverse nesting order; sink/format/type/capacity failures remain errors, not swallowed telemetry loss.
- Provider/tool timing and reported usage are explicit typed fields, not nonexistent special log helpers. Unknown token/cost metadata remains unavailable.
- Sinks are application-owned. File creation/JSON trace-file encoding is not an implicit Cog side effect. Sanitize before callbacks or persistence; diagnostic output must not dump raw secret-bearing session state.
- Callbacks execute on the caller's owner thread and must cooperate with bounded service; no extra transport worker thread is implied.

## Gap analysis and release amendment

The original `cog_obs_log_provider`, `cog_obs_log_tool`, `cog_obs_metrics` and `cog_obs_diag_dump` names are absent. Generic declared APIs cover typed log/span/snapshot delivery; application export formats remain an explicit responsibility. No new convenience APIs are necessary for this flow.

## Executable evidence and required follow-up

Existing baseline test: [`applications/cog-cli/tests/test_18_observability.c`](../../applications/cog-cli/tests/test_18_observability.c). It exists as historical
engine/mock application evidence. It was not rerun in this design-only pass and
does not prove the new mandatory headers, real transport or durable-tool behavior.

Future conformance requirements: Typed mismatch/overflow/sink failure, span nesting, sanitized provider/tool fields, unknown usage, bounded callback behavior under streaming and no implicit file output.

Header tracing is complete only where the gap analysis says so. Production
implementation and executable scenario migration remain gated by pass 3 and the
independent final review; no passing runtime result is inferred from these tables.

## Runtime observability amendment, pass 1

Design `d32fafd` starts from main8c68b4a after K/L. Generic obs APIs are
implemented but the scenario tests still manually emit observations. The
[dedicated runtime contract](../REAL_CLIENT_OBSERVABILITY_CONTRACTS.md) defines
the minimal production bridge; net.h supplies a numeric constructor-borrowed
observer and checked latched status, with no payload/URL/secret pointers.
Implementation and actual-path oracles are delivery `b64ee7c`, serialized after
R12. No runtime passing claim follows from this header amendment.

## Runtime pass 2: typed application facts

The app brackets actual admitted provider/tool work with existing span APIs or
typed duration/status records; sequential CLI work follows stack nesting.
Reported usage has explicit availability and omitted unknown values. Numeric
request/configuration-generation and assistant/ordinal identities survive label
omission. Optional labels use bounded safe copies and availability, so valid
long configuration strings cannot overflow logging or be silently truncated.
Arguments, commands, paths, output, model text, opaque continuation and secrets
never become fields. Sink/clock/format failures are checked and latched; normal
owned drain/persistence precedes failure reporting, without replaying an effect
to regenerate telemetry. The concurrent R12 generations share app/host obs
lifetime but never borrow diagnostic strings from retired configuration storage.
The dedicated contract pins this composition; no new public obs API is needed.

## Runtime pass 3: error and privacy boundary

Timing/log failures stop later app admission through normal owned cleanup, never
make an already completed effect replayable. Diagnostic strings are bridge-owned
copies, separate from replaceable config/secret lifetime. Config-private label
screening exposes only safe bounded labels/availability, never secret storage.
No shared obs stack is used for nonnested simultaneous providers. All callbacks
remain on the owner thread, with reserved storage and checked downstream errors.
Header-only validation establishes contracts/compilation, not runtime conformance.
