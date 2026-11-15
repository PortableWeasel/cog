# R09: Command timeout/cancel, retained output and process-group reap

Stage 14.D pass 2; baseline header pass 1 `29b41e8`, amended by this pass.
This is a contract trace, **not an executable conformance pass**.

## Sequence

```mermaid
sequenceDiagram
    CLI->>Workspace: cog_workspace_new(workspace,config)
    CLI->>Toolkit: cog_toolkit_execution_new(exec,core,table,id,name,args,len,limits)
    CLI->>Toolkit: cog_toolkit_execution_start(exec,ALLOW,now) after durable STARTED
    loop bounded fair service
    CLI->>Toolkit: cog_toolkit_execution_poll(exec,now,done,deadline)
    end
    CLI->>Toolkit: cog_toolkit_execution_cancel(exec,now)
    Note over Toolkit: TERM -> grace deadline -> KILL; drain pipes and reap group
    CLI->>Toolkit: cog_toolkit_execution_result(exec,result,output,capacity,required)
    CLI->>Toolkit: cog_toolkit_execution_free(exec) only after done
```

## Exact public coverage

Each symbol below is declared in the linked public header. Local application
decisions in the diagram are not invented library functions.

| Step | Exact calls, types, events or fields | Declaring header |
|---|---|---|
| Workspace policy/path/environment | `cog_workspace_new`, `cog_workspace_config_t`, `cog_workspace_tools`, `cog_workspace_free` | [libraries/cog-toolkit/include/cog/workspace.h](../../libraries/cog-toolkit/include/cog/workspace.h) |
| Admission/cancel/result lifetime | `cog_toolkit_execution_new`, `cog_toolkit_execution_start`, `cog_toolkit_execution_poll`, `cog_toolkit_execution_cancel`, `cog_toolkit_execution_result`, `cog_toolkit_execution_free` | [libraries/cog-toolkit/include/cog/toolkit_exec.h](../../libraries/cog-toolkit/include/cog/toolkit_exec.h) |
| Tool operations/results | `cog_tool_async_vtable_t`, `cog_tool_async_config_t`, `cog_tool_output_t` | [libraries/cog-core/include/cog/tool.h](../../libraries/cog-core/include/cog/tool.h) |
| Durable intent | `cog_session_tool_transition` | [libraries/cog-core/include/cog/session.h](../../libraries/cog-core/include/cog/session.h) |
| Save before effect | `cog_store_save` | [libraries/cog-core/include/cog/session_store.h](../../libraries/cog-core/include/cog/session_store.h) |


## Ownership and invariants

- run_command requires workspace enable_commands and explicit per-call ALLOW plus durable STARTED. Commands receive an explicit sanitized environment and copied cwd/args; no inherited credential environment.
- Poll never blocks; stdout/stderr capture and traversal work are bounded. Output finalizes only after owned process group is terminated and owned children reaped. Side effects can remain after timeout/cancel and must be reported.
- Active/cancelling execution_free returns INVALID_STATE intact. Result copying may repeat arbitrarily without rerun; workspace cannot free while any execution borrows it.

## Failure and capacity branches

- Test child ignores TERM, forks descendants, floods pipes, exits before cancellation and outlives parent shell. Use deadline-driven KILL and nonblocking reap; no orphan/zombie success.
- File edit followed by huge output and tiny destination must edit once. Capture truncation is terminal/reportable, never an execute retry.

## Evidence and gap disposition

Original06 synchronous timeout test cannot prove responsive async/process-group contracts.

**Required future executable evidence:** I/Q/K: disposable workspace process-tree and output-saturation tests with descriptor/PID leak checks; no paid provider needed.

**Amendment references:** D06,D10 in [pass-2 register](../REAL_CLIENT_SCENARIO_PASS2.md).

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
