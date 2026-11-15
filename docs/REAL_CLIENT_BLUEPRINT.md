# Real-model client: release blueprint

> [Stage 14.E final review](REAL_CLIENT_PASS3.md) passed after the additional
> independent review of `8563f81`. The three header passes and final review are
> complete. A–K/Q and local-agent/replay/assertion primitives are integrated;
> L is in progress and M/N/O/P remain open. See the [current roadmap](REAL_CLIENT_WBS.md).

> [Stage 14.F transport delivery](REAL_CLIENT_F_EVIDENCE.md) is independently
> reviewed and integrated. The initial no-replay profile uses fresh HTTP/1.1
> connections with pooling/HTTP2 disabled. Full project installation and shipped
> dependency closure remain gate P requirements.

> [Stage 14.Q scheduler delivery](REAL_CLIENT_Q_EVIDENCE.md) is independently
> reviewed and integrated. Combined real-library Q/G check `b8a6fc1` is complete
> ([acceptance evidence](../libraries/cog-core/contracts/session_store/CONTRACT.md#independent-acceptance-and-combined-gq-integration)); N/O protocol integration remains open.

Status: implementation through K integrated at `42c5e15`, 2026-11-15. Parent ticket: `2b9d0ea`.
The user authorized autonomous design decisions and testing. This supersedes
Stage 14's earlier scope-confirmation requirement. Header and implementation
gates remain technical requirements, not repeated user approval requests.
The [Stage 14.C contract record](REAL_CLIENT_CONTRACTS.md) specifies public
header pass 1. Scenario pass 2 and pass 3 plus the additional independent final
review are integrated; the accepted contracts include their later amendments.

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
3. Keep libcurl multi a conditional transport candidate, subject to the
   [lifecycle and DNS research](research/https-lifecycle.md) and allocation gate. Its
   process-global initialization must be explicit and host-owned, with no
   Cog global reference counter or lazy initialization. The host initializes
   the dependency before contexts and cleans up after all contexts. If this
   cannot meet the embedding contract, select a different backend at the
   header gate rather than quietly weakening the no-global-state rule.
   The initial supported profile requires c-ares as the primary resolver:
   ENABLE_ARES=ON and ENABLE_THREADED_RESOLVER=OFF, recorded build provenance,
   runtime ASYNCHDNS and ares metadata, c-ares-only DNS_SERVERS support, and
   rejection of hybrid threaded-primary/asyn-rr builds. Fail construction
   clearly when that profile cannot be established. The installed threaded
   resolver is excluded: a controlled stall blocked removal for about 1.9s.
   QUICK_EXIT leaves a resolver thread alive after cleanup and is not an
   embedding solution. Use maintained security-patched dependencies and rerun
   the controlled cancellation suite for the actual shipped profile; empirical
   latency ceilings are not universal scheduling/allocator guarantees.
4. Keep JSON private to provider protocol and toolkit argument/schema validation
   boundaries; expose no parser types. The accepted
   [protocol research](research/provider-wire.md) selects yyjson 0.12.0 over
   pinned jsmn, using explicit caller pools, strict parsing and separate
   quote/escape-aware depth preflight. Parser and writer both use explicit
   allocators; no fallback allocation or ad hoc JSON string concatenation.
   Pass 1 reserves constructor-owned pools and rejects duplicate keys at every
   level. Toolkit separately reserves its schema/parser pool. Dependency packaging
   is implemented for provider/toolkit; final shipped dependency closure remains
   gate P work. The standalone PoC is not library code.
5. Constructor-owned bounded Cog workspaces hold asynchronous request copies,
   JSON state, deltas, sessions and tool results, with no runtime Cog growth.
   Pass 1 explicitly declares constructor-owned dependency handles may allocate
   inside curl/c-ares/TLS during service until matching destruction. Hosts must
   acknowledge dynamic dependency memory; a requested total dependency byte cap
   is unsupported. No per-context quota or constructor-only allocation claim is
   made from the 72 measured polling allocations. Passed header gate E accepted
   this visible ownership profile; F delivery records its implementation checks.
6. Pass 1 defines generic nonblocking poll/cancel/next/get_error, typed deltas,
   usage/finish metadata and the public first-member provider base. next reports
   exact storage capacity without consuming an overflowed item; retained queues
   pause upstream work. The contract record fixes initial names/ABI; pass 2 traced
   and amended call coverage before the completed final review.
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
13. Core scheduling owns fairness independently of transport and CLI handlers.
    Delivery Q (`bcd9c0a`) must service fallible nonblocking hooks at bounded
    dispatch intervals while events remain queued, then bound idle waits by
    the next deadline. Prove timer, IPC and cancellation progress under sustained
    traffic before conversation-loop delivery K; do not introduce core link
    dependencies on provider, net or toolkit implementations.

## Pass 2 contract trace status

Stage D now supplies [all 38 individual traces and the amendment register](REAL_CLIENT_SCENARIO_PASS2.md).
Independent pass 3/E verified the required public-call coverage and amendments.
That header gate does not establish all-38 runtime conformance; the current
[execution roadmap](REAL_CLIENT_WBS.md#verification-plan) separates scoped delivery
evidence, M single-process acceptance and P full-suite release requirements.
REAL defaults to a durable store; explicit no-store is text-only.
Shared-net request-local retention, bounded nonblocking IPC, remote update ingress,
active-display snapshots and quiescent same-host guarded migration are public
contracts, with delivery ownership recorded in that register. Opaque streaming
reconstruction and safe committed-prefix recovery are explicit amendments to the
initial contracts. E accepted the explicit dynamic dependency allocation profile.

## Scenario and header gap register

The rows below preserve the original planning gap list. Stage D now maps each
to an individual sequence/call/type/event trace and re-traces original01–20.
The linked pass-2 register records contract coverage and future executable tests
separately; runtime conformance remains gated.

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

Transport research A (`c5671b5`) is integrated and independently reviewed:
[HTTPS lifecycle report](research/https-lifecycle.md) and
[standalone reproduction](../pocs/https-lifecycle/README.md). Strict builds and
normal plus ASan/UBSan/LSan runs each passed 12 transport cases and 80 injected
curl allocation failures. This accepts research evidence, not a production
backend or the public-header gate. The independently reviewed
[DNS follow-up](research/https-lifecycle.md#dns-followup-c-ares-required-for-the-supported-profile)
(`23ae6c8`, [offline runner](../pocs/https-lifecycle/dns/README.md)) resolves
resolver selection with the c-ares-only supported-profile requirement above.
Local blackhole queries cancelled without residual threads or descriptors;
independent review and integration runs measured sub-millisecond removal,
with the configured 800ms timeout completing near 800ms. The threaded-resolver counterexample and
QUICK_EXIT residual-thread result exclude those alternatives. Tests are
empirical and profile-specific; constructor feature checks and admitted-profile
lifecycle validation are now recorded by F; validation of the shipped artifact remains
gate P work. Gate E resolved the explicit third-party allocation/memory-limit
contract; F records OOM checks. Integrated core scheduling delivery Q is separate
from the synthetic fairness PoC.

Protocol research B (`2e1cddf`) is also integrated and independently reviewed:
[JSON/SSE and OpenRouter report](research/provider-wire.md) and
[offline reproduction](../pocs/provider-wire/README.md). Release and
ASan/UBSan/LSan builds passed all three offline cases, with additional critic
tool-identity and capacity-boundary checks. Fixed-pool parser/writer OOM,
Unicode, strict JSON, depth preflight, fragmented SSE and terminal tool
assembly support the parser recommendation. These results do not establish
recoverable delta capacity/retry semantics, schema validation, or persistence.

The 2026-11-15 synthetic live smoke used exactly two generation requests to
`deepseek/deepseek-v4-flash-0731`, each capped at 512 output tokens, with no
retry or model substitution. Both returned HTTP 200: an identified echo tool
call (`tool_calls`, 309 input / 50 output tokens), then its matching result
and final text (`stop`, 111 input / 4 output tokens). Reported usage cost was
USD 0.00006864 total. Sanitized semantic fixtures preserve tool/result
correlation and repeated finish metadata; they do not preserve transport
fragment boundaries. No further live run is needed for review. Reasoning was
disabled and opaque continuation was absent in this experiment; production
headers now specify bounded owned continuation separately from visible text;
H/G provide component conformance evidence and P retains final coverage checks.

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
The transport PoC uses local fixtures; protocol research performed the bounded
authenticated smoke recorded above. Tests must not rerun that smoke implicitly.

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
were changed by the planning record. Three actual header passes plus the extra
full review subsequently passed at E; these earlier planning passes did not
replace them. Later contract amendments still require trace and review gates.
