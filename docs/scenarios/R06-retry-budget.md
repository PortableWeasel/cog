# R06: 429, finite application backoff and exhaustion

Stage 14.D pass 2; baseline header pass 1 `29b41e8`, amended by this pass.
This is a contract trace, **not an executable conformance pass**.

## Sequence

```mermaid
sequenceDiagram
    CLI->>Provider: cog_provider_poll(provider,now,deadline)
    CLI->>Provider: cog_provider_next(provider,FINISH,storage,capacity,required)
    CLI->>Provider: cog_provider_get_error(provider,error)
    CLI->>CLI: if replay_safe AND no accepted output/effects AND budget remains, arm timer
    loop sustained queued events during backoff
    Core->>CLI: cog_service_fn(arg,now,next_deadline)
    end
    CLI->>Provider: cog_provider_send(provider,copied_request) at deadline only
    Note over CLI,Provider: default one additional attempt; net never retries
```

## Exact public coverage

Each symbol below is declared in the linked public header. Local application
decisions in the diagram are not invented library functions.

| Step | Exact calls, types, events or fields | Declaring header |
|---|---|---|
| Error/terminal access | `cog_provider_poll`, `cog_provider_next`, `cog_provider_get_error`, `cog_provider_send` | [libraries/cog-provider/include/cog/provider_vtable.h](../../libraries/cog-provider/include/cog/provider_vtable.h) |
| Retry metadata | `cog_provider_error_t`, `cog_provider_delta_t` | [libraries/cog-provider/include/cog/provider_request.h](../../libraries/cog-provider/include/cog/provider_request.h) |
| Deadline fairness | `cog_service_fn`, `cog_core_run`, `cog_core_config_t` | [libraries/cog-core/include/cog/core.h](../../libraries/cog-core/include/cog/core.h) |
| Single budget/presence | `cog_cli_options_t`, `COG_CLI_OPT_RETRIES`, `COG_CLI_OPT_RETRY_MS` | [applications/cog-cli/include/cog_cli/cli.h](../../applications/cog-cli/include/cog_cli/cli.h) |
| HTTP vs transfer error | `cog_net_response_t`, `cog_net_config_t` | [libraries/cog-net/include/cog/net.h](../../libraries/cog-net/include/cog/net.h) |


## Ownership and invariants

- HTTP 429 is not a transport failure; provider classifies sanitized error and parsed retry delay. retryable by itself is insufficient: app independently rejects replay after accepted output or any tool effects.
- Net max_retries and retry_backoff_ms are zero. Default max_retries=1 means two total attempts, not one at every layer. Explicit zero is representable and stops after first failure.
- Deadline=min(backoff,remaining retry elapsed,remaining turn). Service continues cancel/timer/IPC work under an always-nonempty event queue; no blocking sleep.

## Failure and capacity branches

- Repeated 429 reaches a final error within both attempt and elapsed bounds. Invalid Retry-After uses bounded app policy, never an unbounded delay.
- Cancellation during backoff prevents another send. Fatal hook error propagates; AGAIN is normal no progress.

## Evidence and gap disposition

Original12 mock retry test is historical; https-lifecycle synthetic fairness is research, not production core proof.

**Required future executable evidence:** Q/K: continuous-queue, explicit zero retry, 429/exhaustion and cancel-at-deadline tests with exact send counts.

**Amendment references:** D02,D09,D10 in [pass-2 register](../REAL_CLIENT_SCENARIO_PASS2.md).
