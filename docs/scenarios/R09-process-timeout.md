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
