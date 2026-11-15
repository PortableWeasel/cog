# Scenario 15: Multi-turn tool use with durable at-most-once admission

Stage 14.D pass-2 retrace from public-contract baseline `29b41e8`.
This replaces the earlier schematic SVG as the normative call trace. The SVG is
retained as historical design material and is not current API coverage.

## Sequence diagram

```mermaid
sequenceDiagram
    participant App
    participant Provider
    participant Session
    participant Store
    participant Toolkit
    Provider-->>App: TOOL_BEGIN / ARGUMENTS / END then successful FINISH
    App->>Session: append one assistant with stable IDs and opaque continuation
    App->>Toolkit: cog_toolkit_execution_new(complete validated arguments)
    alt Policy denies or no durable admission available
      App->>Session: transition PROPOSED to DENIED, append identified denial result
    else Explicit ALLOW
      App->>Session: transition PROPOSED to STARTED
      App->>Store: cog_store_save(STARTED intent)
      Store-->>App: durable success required
      App->>Toolkit: cog_toolkit_execution_start(ALLOW,now)
      App->>Toolkit: poll to terminal; copy result (retry capacity only)
      App->>Session: final ledger and TOOL message with tool_call_id
      App->>Store: save result and final ledger together
    end
    App->>Provider: send history with matching results and exact opaque data
```

## Exact public-header coverage

Names below are exact declared functions, types, enum members or struct fields;
application-only policy/envelope steps are identified explicitly rather than
presented as library calls. Each header path names the declaring public file.

| Trace step | Exact declaration / member | Declaring header |
|---|---|---|
| Typed call assembly | `cog_provider_delta_t`; `COG_PROVIDER_DELTA_TOOL_BEGIN`; `COG_PROVIDER_DELTA_TOOL_ARGUMENTS`; `COG_PROVIDER_DELTA_TOOL_END`; `COG_FINISH_REASON_TOOL_CALLS` | [libraries/cog-provider/include/cog/provider_request.h](../../libraries/cog-provider/include/cog/provider_request.h) |
| OpenRouter fragmented identity | `cog_openrouter_config_t`; `cog_openrouter_provider_vtable` | [libraries/cog-provider/include/cog/openrouter.h](../../libraries/cog-provider/include/cog/openrouter.h) |
| Durable default / explicit text-only mode | `cog_cli_store_mode_t`; `COG_CLI_STORE_DEFAULT`; `COG_CLI_STORE_DURABLE`; `COG_CLI_STORE_DISABLED`; `cog_cli_options_t.store_mode`; `cog_cli_options_t.store_dir`; `COG_CLI_OPT_STORE_MODE`; `COG_CLI_OPT_STORE_DIR` | [applications/cog-cli/include/cog_cli/cli.h](../../applications/cog-cli/include/cog_cli/cli.h) |
| Complete terminal delivery | `cog_provider_next`; `cog_provider_get_error` | [libraries/cog-provider/include/cog/provider_vtable.h](../../libraries/cog-provider/include/cog/provider_vtable.h) |
| Identified durable message | `cog_message_tool_call_t`; `cog_message_t.tool_call_id`; `cog_message_opaque_t` | [libraries/cog-core/include/cog/message.h](../../libraries/cog-core/include/cog/message.h) |
| Atomic insertion / execution key | `cog_session_append`; `cog_session_tool_transition`; `cog_session_tool_state` | [libraries/cog-core/include/cog/session.h](../../libraries/cog-core/include/cog/session.h) |
| STARTED durable boundary | `cog_store_save` | [libraries/cog-core/include/cog/session_store.h](../../libraries/cog-core/include/cog/session_store.h) |
| Validate / policy / execute / retained output | `cog_toolkit_execution_new`; `cog_toolkit_execution_start`; `cog_toolkit_execution_poll`; `cog_toolkit_execution_result` | [libraries/cog-toolkit/include/cog/toolkit_exec.h](../../libraries/cog-toolkit/include/cog/toolkit_exec.h) |
| Explicit policy | `COG_TOOLKIT_POLICY_UNDECIDED`; `COG_TOOLKIT_POLICY_DENY`; `COG_TOOLKIT_POLICY_ALLOW` | [libraries/cog-toolkit/include/cog/toolkit_exec.h](../../libraries/cog-toolkit/include/cog/toolkit_exec.h) |
| Side effects / truncation | `cog_toolkit_result_t.tool_call_id`; `cog_toolkit_result_t.side_effects_possible`; `cog_toolkit_result_t.truncated` | [libraries/cog-toolkit/include/cog/toolkit_result.h](../../libraries/cog-toolkit/include/cog/toolkit_result.h) |
| Owned continuation history / next request | `cog_session_copy_history` | [libraries/cog-core/include/cog/session.h](../../libraries/cog-core/include/cog/session.h) |
| Copied request | `cog_provider_send` | [libraries/cog-provider/include/cog/provider_vtable.h](../../libraries/cog-provider/include/cog/provider_vtable.h) |

## Ownership, errors and adversarial branches

- Fragmented tool IDs/names are privately buffered through wire-turn validation: `id="call_"` then `id="42"`, `name="ed"` then `name="it"`, interleaved with argument fragments, yields exactly one TOOL_BEGIN with `call_42`/`edit`, then ARGUMENTS and TOOL_END. The first argument fragment does not establish identity completion.
- Fragmented tool arguments never execute. Provider validates identity and strict JSON; toolkit independently validates complete arguments/schema and resolves a static descriptor without effects.
- REAL without `--store` creates a durable file store at absolute `$XDG_STATE_HOME/cog`, otherwise absolute `$HOME/.local/state/cog`; missing location/open failure is explicit. `--no-store` is deliberate text-only mode: tools omitted and unsolicited tool calls denied, even under ALLOW policy. A null backend never satisfies durable intent.
- ALLOW requires durable STARTED save before start. Failure does not fall back to unsaved execution. A crash anywhere after that save loads as indeterminate, never automatic rerun.
- Tool result copying after output OVERFLOW is repeatable; the tool start counter and filesystem effect remain one. Final ledger/result save must succeed before another model request.
- Denial and invalid call produce explicit identified outcomes without execution. Unknown opaque format or missing required continuation fails before effects.
- The next request replays one assistant with its call list, matching TOOL results, and separately owned opaque continuation; it does not synthesize unidentified legacy tool nodes.

## Gap analysis and release amendment

**O15-G1 resolved by pass-2 CLI store-mode amendment:** REAL now defaults to a durable XDG/HOME file store; explicit DISABLED is text-only and cannot grant ALLOW. This resolves the pass-1 no-store contradiction without an unsafe unsaved-execution bypass. Fragmented ID/name assembly is also explicit in the OpenRouter contract. No remaining contract gap is claimed for this tool loop; implementation conformance remains gated.

## Executable evidence and required follow-up

Existing baseline test: [`applications/cog-cli/tests/test_15_multi_turn_tools.c`](../../applications/cog-cli/tests/test_15_multi_turn_tools.c). It exists as historical
engine/mock application evidence. It was not rerun in this design-only pass and
does not prove the new mandatory headers, real transport or durable-tool behavior.

Future conformance requirements: Split ID/name/arguments, policy denial, no-store default, failed STARTED save, crash after effect before result save, repeated result-copy overflow and exact opaque continuation request replay.

Header tracing is complete only where the gap analysis says so. Production
implementation and executable scenario migration remain gated by pass 3 and the
independent final review; no passing runtime result is inferred from these tables.

## Pass-3 parser/process bounds retrace (E03)

`cog_tool_async_config_t` now fixes argument/schema bytes, JSON depth and codec
workspace capacity ([tool.h](../../libraries/cog-core/include/cog/tool.h)).
Construction rejects over-limit input before effects, duplicate keys and invalid
UTF-8. Scalar-count string lengths, finite numeric semantics and unsupported
schema keywords are pinned in [toolkit_exec.h](../../libraries/cog-toolkit/include/cog/toolkit_exec.h).
No remote schema fetch or hidden parser growth occurs. Process-tree supervision
keeps subreaper/signal changes inside an owned helper, never the embedding host;
the cancellation profile requires descendants remain in the owned group. Test
ordinary inherited-group grandchildren outliving the shell, exact parser/depth
limits, unsupported numeric ranges and result resize with one effect.
