# R05: Denied policy, invalid schema and unsolicited tools

Stage 14.D pass 2; baseline header pass 1 `29b41e8`, amended by this pass.
This is a contract trace, **not an executable conformance pass**.

## Sequence

```mermaid
sequenceDiagram
    CLI->>Toolkit: cog_toolkit_execution_new(exec,core,table,id,name,args,len,limits)
    alt valid prepared call and policy denies
    CLI->>Toolkit: cog_toolkit_execution_start(exec,DENY,now)
    CLI->>Toolkit: cog_toolkit_execution_result(exec,result,output,capacity,required)
    else malformed/unsupported schema before constructor succeeds
    CLI->>CLI: build identified error result without execution handle
    end
    CLI->>Session: cog_session_tool_transition(PROPOSED,DENIED)
    CLI->>Session: cog_session_append(session,TOOL_result,node)
    CLI->>Store: cog_store_save(store,session) before next send
```

## Exact public coverage

Each symbol below is declared in the linked public header. Local application
decisions in the diagram are not invented library functions.

| Step | Exact calls, types, events or fields | Declaring header |
|---|---|---|
| Strict schema/denial | `cog_toolkit_execution_new`, `cog_toolkit_execution_start`, `cog_toolkit_execution_result`, `COG_TOOLKIT_POLICY_DENY`, `COG_TOOLKIT_POLICY_UNDECIDED` | [libraries/cog-toolkit/include/cog/toolkit_exec.h](../../libraries/cog-toolkit/include/cog/toolkit_exec.h) |
| Declared table/context | `cog_tool_t`, `cog_tool_async_config_t` | [libraries/cog-core/include/cog/tool.h](../../libraries/cog-core/include/cog/tool.h) |
| Ledger and result | `cog_session_tool_transition`, `cog_session_append`, `COG_TOOL_EXECUTION_DENIED` | [libraries/cog-core/include/cog/session.h](../../libraries/cog-core/include/cog/session.h) |
| Identity/status | `cog_message_t`, `COG_MESSAGE_ROLE_TOOL` | [libraries/cog-core/include/cog/message.h](../../libraries/cog-core/include/cog/message.h) |
| Disabled persistence mode | `COG_CLI_STORE_DISABLED`, `COG_CLI_TOOL_POLICY_ASK` | [applications/cog-cli/include/cog_cli/cli.h](../../applications/cog-cli/include/cog_cli/cli.h) |
| Durable history | `cog_store_save` | [libraries/cog-core/include/cog/session_store.h](../../libraries/cog-core/include/cog/session_store.h) |


## Ownership and invariants

- UNDECIDED is never ALLOW; noninteractive ASK denies. Invalid schema/unsupported keyword/unknown tool never invokes start.
- DENY is a no-effect terminal outcome. It does not need a STARTED save; save denial+identified result before next request when durable mode is enabled. No-store mode keeps denial only in volatile history.
- Malformed wire argument JSON fails the turn before this trace (R15); syntactically valid but schema-invalid arguments can receive an explicit tool error result.

## Failure and capacity branches

- Test traversal/symlink/absolute paths and disabled command descriptor before effects. No unsolicited tool can bypass no-store mode, absent workspace root or command enablement.
- OVERFLOW copying a denied result repeats only copying; constructor NOMEM leaves *exec=NULL and zero effects.

## Evidence and gap disposition

Existing original15 covers baseline tool result only; new denial/schema/policy contracts are not implemented yet.

**Required future executable evidence:** I/K: denied/invalid/UNDECIDED/ASK/no-store matrix with side-effect counter exactly zero.

**Amendment references:** D01,D06 in [pass-2 register](../REAL_CLIENT_SCENARIO_PASS2.md).
