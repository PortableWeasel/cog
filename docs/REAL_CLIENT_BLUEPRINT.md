# Real-model client: release blueprint

Status: planning baseline, 2026-11-15. Parent ticket: `2b9d0ea`.
The user authorized autonomous design decisions and testing. This supersedes
Stage 14's earlier scope-confirmation requirement. Header and implementation
gates remain technical requirements, not repeated user approval requests.

## Release boundary and goals

Deliver a Linux/POSIX CLI coding agent and embeddable C11 libraries. A user
can configure OpenRouter, converse with a real model, let it read/search/edit
workspace files and execute permitted commands, cancel work, and resume or
fork a persisted conversation. Preserve the existing IPC/subagent libraries
and integrate a bounded headless/subagent workflow before the full release.
Ship text and structured tool calls first. Defer GUI/web clients, multimodal
input, remote-host scheduling, automatic context summarization, and a broad
provider matrix. These are follow-on scope, not silent release requirements.

| Goal | Owner | Acceptance evidence |
|---|---|---|
| Stream real conversations | provider + net + CLI | deterministic wire fixtures and bounded live smoke |
| Complete coding tasks | CLI + toolkit | edit/build task in disposable workspace |
| Resume and fork safely | session/store + CLI | restart and branch-isolation tests |
| Stay responsive | core + net + CLI | cancel during DNS/connect/stream/backoff/tool execution |
| Respect execution policy | CLI + tools | denied calls never execute; explicit autonomous policy |
| Run headless and delegate | IPC + orchestrator + application | reconnect/replay and child result workflow |
| Embed without implicit startup | all libraries | external consumer and lifecycle tests |

## Functional boundaries and decisions

1. Keep the existing eight libraries. Protocol decoding belongs to
   cog-provider; HTTP/TLS and transport scheduling belong to cog-net. CLI
   owns user interaction, policy decisions, and application composition.
2. Keep static const dispatch/vtables. Endpoint configuration is caller-owned
   immutable data per configuration generation; do not mutate a static
   descriptor table or dynamically register provider implementations.
   In-flight instances retain their generation until teardown.
3. Prefer libcurl multi for transport, subject to the lifecycle PoC. Its
   process-global initialization must be explicit and host-owned, with no
   Cog global reference counter or lazy initialization. The host initializes
   the dependency before contexts and cleans up after all contexts. If this
   cannot meet the embedding contract, select a different backend at the
   research gate rather than quietly weakening the no-global-state rule.
4. Keep JSON behind a private provider boundary. Compare a mature parser with
   caller-supplied allocator/arena support against a small token parser.
   Decide from escaping, Unicode, depth/size limits, OOM behavior, license,
   packaging, and cleanup evidence. No ad hoc string concatenation for JSON.
5. Constructor-owned bounded workspaces hold asynchronous request copies,
   parser state and deltas. send() must not retain borrowed request pointers.
   Document third-party allocations and explicit handle lifetimes; measure
   whether they satisfy the allocation contract before adopting a backend.
6. Add provider-neutral polling, cancellation, error metadata, typed content
   and tool deltas, usage/finish metadata. Names and ABI are to be settled in
   headers, not inferred from this document. Overflow must not consume an
   unrecoverable delta; specify required-capacity and retry semantics.
7. Assemble one assistant message per completed model turn, with identified
   tool calls. Streaming display is separate from durable conversation
   history. Preserve provider-required opaque continuation metadata when
   needed; never conflate it with visible text or request model reasoning.
8. One application-owned retry budget: default one retry, bounded elapsed
   time, timer-driven backoff. Transport must not multiply retries. Retry
   only before accepted output/tool side effects; never replay an executed
   tool automatically. Mid-stream failures preserve an explicit incomplete
   turn and require a new user-authorized continuation/retry.
9. Tools execute only after complete validated arguments and policy checks.
   Start sequentially; concurrency is opt-in for reentrant independent tools.
   Bound output, runtime, tool rounds and model output. Command cancellation
   terminates/reaps the owned process group. Workspace path checks cover
   symlinks and traversal; command permission is not an OS sandbox claim.
10. CLI offers interactive decisions and explicit noninteractive allow/deny
    policy. This project's preapproved tests run in disposable workspaces;
    that authorization is not an unconditional permission default for users.
11. Config precedence: CLI > environment > config file > defaults. Credential
    references resolve at startup into memory; never store credentials in
    sessions, logs, fixtures or ticket bodies. Errors are sanitized before
    persistence. No implicit fallback to mock when real setup fails.
12. Version persistence format before structured-message changes. Specify
    legacy load/migration, atomic save, truncated-tail recovery and refusal
    of unsupported versions. Persist unfinished tool status so resume cannot
    duplicate side effects. Context overflow fails clearly; no silent history
    truncation or billing guesses when usage is unavailable.

## Scenario and header gap register

These are planning traces, not a claim of header coverage. Each row becomes
an individual sequence diagram, call-to-header table and executable test in
the header/scenario work package. Re-trace all existing scenarios 01–20 too.

| ID | Flow | Existing contract anchor | Required amendment / assertion |
|---|---|---|---|
| R01 | config → credential → startup | config.h, endpoint.h | resolution, lifetime, redaction |
| R02 | prompt → HTTPS → SSE → display → save | net.h, provider_request.h, session.h | complete-turn assembly |
| R03 | fragmented bytes → UTF-8/JSON/tool delta | provider_request.h | incremental typed decoding, bounded buffers |
| R04 | tool delta → complete arguments → policy → exec → result → model | message.h, tool.h | stable call IDs and schemas |
| R05 | deny/invalid tool → result → model | toolkit headers | no execution; explicit result |
| R06 | 429 → timer → retry exhaustion | provider_vtable.h, core.h | generic error accessor and finite budget |
| R07 | partial output → network loss | net.h, session.h | no transparent duplicate output/replay |
| R08 | cancel during connect/stream/backoff | provider_vtable.h, net.h | poll deadline and cancellation |
| R09 | command timeout/cancel → reap | toolkit headers | process group ownership, bounded output |
| R10 | save → restart → resume | session_store.h, message.h | versioned structured messages |
| R11 | fork → divergent tools/history | session.h | ancestor isolation, call-ID scope |
| R12 | reload config during request | config.h, endpoint.h | generation ownership and rollback |
| R13 | UI detach → buffered events → replay | ipc.h, core.h | bounded replay, gap indication |
| R14 | parent → child → result/crash/cancel | agent.h, orchestrator.h | bounded placement and no duplicate effects |
| R15 | malformed/oversize data → error | net.h, provider_request.h | no partial commit, no secrets |
| R16 | shutdown with active work | core.h, net.h, toolkit headers | bounded cleanup and resumable status |
| R17 | model context/round/token limits | provider_request.h, CLI header | deterministic stop and useful diagnostics |
| R18 | install → external embedder → cleanup | exported target sets | link closure, explicit dependency lifecycle |

## Research evidence and open experiments

Reviewed 2026-11-15:
- [libcurl API overview](https://curl.se/libcurl/c/libcurl.html): multi interface
  supports concurrent transfers on one thread.
- [libcurl lifecycle](https://curl.se/libcurl/c/curl_global_init.html) and
  [thread safety](https://curl.se/libcurl/c/threadsafe.html): explicit global
  initialization requires an embedding decision, not hidden Cog state.
- [OpenRouter streaming](https://openrouter.ai/docs/api_reference/streaming):
  streams require event framing and comment handling. Freeze the chosen
  chat-completions protocol with fixtures; do not mix Responses API events.
- [DeepSeek V4 Flash 0731](https://openrouter.ai/deepseek/deepseek-v4-flash-0731):
  candidate test model `deepseek/deepseek-v4-flash-0731`; revalidate model
  availability and tool support from the catalog before each live gate.

OpenRouter base URL: https://openrouter.ai/api/v1. Live smoke uses synthetic
prompts and a disposable workspace, max 3 requests per smoke run, max 512
output tokens per request, no automatic model substitution or retry storm.
Record model/date/status/usage without credentials or private source text.
Testing is preapproved. pass list has no OpenRouter entry, but the user identified pi login storage.
The OpenRouter entry in ~/.pi/agent/auth.json is accessible. Read its key only
into the test process memory; never print it or pass it as a shell argument.
No authenticated provider test or PoC has been performed in this planning pass.

## Three planning passes and final review

Pass 1 — scope/blocks: mapped release goals to existing libraries; excluded
GUI/multimodal expansion; chose the first provider and practical coding tools.
Pass 2 — scenario tracing: inspected message.h, provider_request.h,
provider_vtable.h and net.h. Found missing generic cancellation/error access,
structured tool identity, asynchronous ownership, complete-turn assembly,
retry exhaustion and persistence migration contracts. Added R01–R18.
Pass 3 — consistency/edge cases: resolved retry ownership, side-effect replay,
config generation lifetime, dependency initialization, tool policy and live
budget. Made all implementation packages depend on a real header gate.
Final planning review: preserve historical Stage 12 evidence; do not describe
its mock gate as real-product completion. No public headers or implementation
are changed here. Three actual header passes plus an extra full review remain
mandatory before implementation; these planning passes do not replace them.
