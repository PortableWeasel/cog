# R10 · Background job across foreground prompts

Header-stage trace for `5f60ee4`. The [standalone PoC](../../pocs/tool-use/README.md)
verified the provider round trip and Linux direct-child behavior. The future
runtime tests exercise guarded durability and the workspace helper.

```mermaid
sequenceDiagram
    participant H as Embedded host
    participant R as Runtime
    participant M as Model
    participant S as Guarded store
    participant P as Workspace helper
    H->>R: Submit prompt
    R->>M: Exact eligible start/status templates
    M-->>R: Complete start call A
    R->>S: Assistant and PROPOSED
    R->>R: Validate schema, policy and cwd
    R->>S: A STARTED + job INTENT
    R->>P: Start under the owned guard
    P-->>R: Supervised execution accepted
    R->>S: Receipt for A + A SUCCEEDED + job ACCEPTED
    R->>M: Correlated receipt for A
    M-->>R: Final answer
    H->>R: Submit unrelated prompt while job is live
    R->>M: New user turn on same branch
    M-->>R: Answer while job is live
    P-->>R: Output and confirmed reap
    R->>S: Job FINISHED
    H->>R: Submit status prompt
    M-->>R: New status call B
    R->>M: Result for B with committed job output
```

| Pass | Trace | Contract and expected observation |
| --- | --- | --- |
| 1 | Advertisement and dispatch, [15](15-multi-turn-tools.md) | Runtime resolves an original descriptor once per pinned context. The provider sees those same names and schemas. V1 background descriptors are absent with disabled configuration, unsupported roles, or a missing capability. |
| 1 | Invalid call, [R05](R05-tool-denial.md) | `cog_toolkit_validate` runs against exact complete arguments before ASK, scope reads, constructor or effects. Malformed input receives a correlated no-effect failure. The standalone toolkit constructor repeats validation. |
| 1 | Allowed launch, [R04](R04-tool-round.md) | `cog_session_tool_job_intent` atomically pairs job intent with STARTED. A committed save is required before toolkit start. Without pidfd/control ownership, the command does not start. Accept documents exactly one correlated receipt and carries no command-success claim. |
| 1 | Worker exits early, [R09](R09-process-timeout.md) | A helper accepted before an exec error still gets an accepted receipt. The job eventually reports failure. The next foreground call observes the committed outcome, even if no running status was observed. |
| 2 | Prompt while running, [20](20-interrupt-and-queue.md) | `cog_runtime_step` continues servicing and reporting deadlines with `input_ready=true`. Fresh input on the same path proceeds. No second model tool result is sent under the launch call ID. |
| 2 | Status and forged IDs, [14](14-conversation-fork.md) | `cog_runtime_job` checks the receipt ancestor and origin binding; inaccessible and absent IDs produce the same failure. Status requires current tool policy. Fork is deferred until durable drain. |
| 2 | Scoped changes, [08](08-config-reload.md), [P01](P01-instruction-support.md) | The job holds its original workspace, binding and guard. Reload returns AGAIN before retaining a candidate. A call that would change scope returns an identified no-effect refusal while the job is live. |
| 2 | Backpressure, [11](11-ui-reconnect.md) | The embedded retained publication gates ordinary new input but cannot stop job polling, timeout, cancellation or terminal save. The offline job records have separate reserved publication storage. |
| 2 | Cancel and shutdown, [05](05-graceful-shutdown.md), [R09](R09-process-timeout.md) | Foreground cancel leaves an accepted job owned. Job cancel or shutdown drives TERM/KILL, bounded drain and actual reap. Close and done remain unavailable until owned cleanup is complete. |
| 3 | Fault saving intent or receipt, [13](13-session-persistence.md) | Intent save failure starts zero commands. Receipt save failure freezes sends, cancels/drains any owned helper and exposes no uncommitted receipt. Restart loads the authoritative generation and reconciles any unresolved pair. |
| 3 | Crash after receipt, [L01](L01-terminal-reconcile-identity.md) | V4 INSPECT exposes the accepted job as indeterminate. Normal acquisition refuses it even though the launch call is closed. Fenced recovery resolves only the job outcome and preserves the original receipt bytes. |
| 3 | Crash before receipt or after terminal save, [17](17-session-migration.md) | Uncertain intent resolves atomically with the unclosed launch. A terminal job restores without re-execution. Handoff requires quiescence; V1–V3 records restore with zero jobs. |
| 3 | Capacity and storage fault | Exhaust job, output, evidence, audit or serialized checkpoint capacity before launch. Later foreground writes cannot consume terminal-commit reservation. On terminal-save failure, retain the evidence, freeze new admission and never expose the candidate as durable. |

The new runtime scenarios are executable-test requirements for the writer phase.
