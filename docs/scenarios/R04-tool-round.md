# R04: Complete tool call, durable intent, policy, effect and result

Stage 14.D pass 2; baseline header pass 1 `29b41e8`, amended by this pass.
This is a contract trace, **not an executable conformance pass**.

## Sequence

```mermaid
sequenceDiagram
    CLI->>Provider: cog_provider_next(provider,delta,storage,capacity,required)
    CLI->>Session: cog_session_append(session,assistant_with_calls,node)
    CLI->>Toolkit: cog_toolkit_execution_new(exec,core,table,id,name,args,len,limits)
    CLI->>CLI: explicit policy decision
    CLI->>Session: cog_session_tool_transition(PROPOSED,STARTED)
    CLI->>Store: cog_store_save(store,session)
    CLI->>Toolkit: cog_toolkit_execution_start(exec,ALLOW,now)
    CLI->>Toolkit: cog_toolkit_execution_poll(exec,now,done,deadline)
    CLI->>Toolkit: cog_toolkit_execution_result(exec,result,output,capacity,required)
    CLI->>Session: append identified TOOL result; transition STARTED to final
    CLI->>Store: cog_store_save(store,session)
    CLI->>Provider: cog_provider_send(provider,history_request)
```

## Exact public coverage

Each symbol below is declared in the linked public header. Local application
decisions in the diagram are not invented library functions.

| Step | Exact calls, types, events or fields | Declaring header |
|---|---|---|
| Terminal validation | `cog_provider_delta_t`, `COG_FINISH_REASON_TOOL_CALLS`, `COG_PROVIDER_DELTA_FINISH` | [libraries/cog-provider/include/cog/provider_request.h](../../libraries/cog-provider/include/cog/provider_request.h) |
| Assemble identity/continuation | `cog_message_t`, `cog_message_tool_call_t`, `cog_message_opaque_t`, `COG_MESSAGE_ROLE_TOOL` | [libraries/cog-core/include/cog/message.h](../../libraries/cog-core/include/cog/message.h) |
| Ledger/association | `cog_session_append`, `cog_session_tool_transition`, `cog_session_tool_state`, `COG_TOOL_EXECUTION_STARTED`, `COG_TOOL_EXECUTION_INDETERMINATE` | [libraries/cog-core/include/cog/session.h](../../libraries/cog-core/include/cog/session.h) |
| Durable boundary and guard | `cog_store_save`, `cog_store_load_ex`, `cog_store_owner_acquire`, `cog_store_owner_guard_borrow`, `cog_store_owner_guard_return`, `cog_store_owner_release` | [libraries/cog-core/include/cog/session_store.h](../../libraries/cog-core/include/cog/session_store.h) |
| Prepare/admit/poll/copy | `cog_toolkit_execution_new`, `cog_toolkit_execution_start`, `cog_toolkit_execution_poll`, `cog_toolkit_execution_result`, `COG_TOOLKIT_POLICY_ALLOW` | [libraries/cog-toolkit/include/cog/toolkit_exec.h](../../libraries/cog-toolkit/include/cog/toolkit_exec.h) |
| Next request copy | `cog_provider_send` | [libraries/cog-provider/include/cog/provider_vtable.h](../../libraries/cog-provider/include/cog/provider_vtable.h) |


## Ownership and invariants

- Execution key is (assistant node ID,call ID), never tool name. Schema validation/construction has no effects. Tool admission waits for successful terminal wire validation and policy.
- In require_owner mode execution_new borrows the matching live guard until free; legacy synchronous execution is UNSUPPORTED and cannot bypass it. Persist STARTED before ALLOW start. Crash before save: no invocation; after successful save but before invocation: load INDETERMINATE; during/after effect before result commit: same; after final durable result: replay result without invoking again.
- After a side effect, a too-small destination retries execution_result only. Retained capture/truncation never authorizes another start. Result+terminal ledger must be durably committed before another provider send.
- If session result insertion/save fails after an effect, retain the execution result, mark/keep unresolved state and stop continuation; do not free the only result and execute again.

## Failure and capacity branches

- No-store mode omits tool schemas and cannot satisfy STARTED precondition. Null-backend save is UNSUPPORTED. Disk error or denied policy causes zero invocations.
- Internal capture limit may mean effects occurred; output.truncated/side_effects_possible makes that explicit. Destination OVERFLOW changes required lengths only and is safely repeatable.

## Evidence and gap disposition

Original06/15 and protocol live tool/result fixture cover baseline correlation only; existing toolkit implementation still requires at-most-once migration.

**Required future executable evidence:** G/H/I/K: injected counter/file-edit result resize, crash at all four save boundaries, result persistence failure and no-store admission tests.

**Amendment references:** D01,D06,D08 in [pass-2 register](../REAL_CLIENT_SCENARIO_PASS2.md).

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

## Independent final-review revision: committed identity (E08)

Discard/reset cannot remove identified/admitted/effect-bearing/fork-shared state.
Safe disposal remains limited to uncommitted text drafts, without rewinding node
IDs. Save/append compare the locked committed generation even if the caller
constructs an empty replacement with the same session ID: committed call/assistant/
ancestor identity, arguments, opaque data, results and prior audit evidence cannot
be removed/rebound or rolled backward. Only documented monotonic ledger progress
is permitted; final results are immutable and INDETERMINATE closes only through
explicit recovery. Newly indeterminate work halts new admission under an already
held guard as well as during acquisition. Failed/uncertain save never makes prior
intent erasable. Test discard/reset/replacement-save against a saved STARTED leaf
with no result, and against a completed result/audit; expect refusal and zero reruns.

## Independent final-review revision: active path and unstarted work (E09/E11)

While any call/result is unresolved, `cog_session_fork` and arbitrary-parent
`cog_session_add_message` cannot switch to a safe sibling or ancestor. Only a
matching TOOL result at the active leaf, or atomic reconcile_tool, may extend
that path. REPLAY load rejects off-active unresolved state; no recovery target
can be stranded. An incomplete text-only turn without tool ledger still permits
an explicit fork from its safe ancestor. Crash oracle: create a safe sibling,
then save active STARTED; attempted sibling switch must fail before the crash,
and restored INDETERMINATE remains reachable for reconciliation.

PROPOSED is unresolved lifecycle state but proves no STARTED admission happened:
normal guard acquisition may resume that current policy/denial phase. It cannot
start unrelated model/user/branch work. Crashed STARTED becomes INDETERMINATE and
requires recovery. Cancelled unstarted proposals receive identified no-effect
DENIED results; they are never mistaken for uncertain executed effects. Test the
crash before STARTED save and cancel-after-proposal boundaries with zero effects.

## H deterministic provider fixture amendment

The narrow [H mock contract](../REAL_CLIENT_H_MOCK_AMENDMENT.md) adds
`cog_mock_provider_new_deltas` in `cog_cli/mock_provider.h`. It supplies retained
TOOL_BEGIN/ARGUMENTS/END/OPAQUE/FINISH fixtures through the same generic callbacks
as the real adapter. K can exercise its tool admission loop without wire calls;
this constructor itself executes no tool and grants no durable admission.
