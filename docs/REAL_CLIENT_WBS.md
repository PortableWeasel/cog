# Stage 14 execution roadmap

> [Stage 14.E final review](REAL_CLIENT_PASS3.md) passed after the additional
> independent review of `8563f81`. The three header passes and final review are
> complete. A–K and Q are integrated; L is in progress. M/N/O/P remain open.

Parent: `2b9d0ea`. This is the current real-client roadmap; Stage 12 remains
historical engine/mock-CLI delivery evidence. User delegates routine design
and preapproves autonomous testing. No pending scope-confirmation gate.

## Dependency-ordered work

The roadmap contains 17 packages (A–Q); Q is placed beside its prerequisite
header gate because it must complete before K.

| Package | Ticket | Deliverable / evidence | Status | Prerequisites |
|---|---|---|---|---|
| A | `c5671b5` | [HTTPS lifecycle research](research/https-lifecycle.md) | Integrated | — |
| B | `2e1cddf` | [JSON/SSE wire research](research/provider-wire.md) | Integrated | — |
| C | `5df3ba1` | [Public headers pass 1](REAL_CLIENT_CONTRACTS.md) | Integrated | A,B |
| D | `345a536` | [Scenario pass 2 and amendments](REAL_CLIENT_SCENARIO_PASS2.md) | Integrated | C |
| E | `feb17b6` | [Header pass 3 and independent final review](REAL_CLIENT_PASS3.md) | Passed | D |
| Q | `bcd9c0a` | [Fair core service scheduling](REAL_CLIENT_Q_EVIDENCE.md) | Integrated | E |
| F | `8f55808` | [Transport HTTPS and polling](REAL_CLIENT_F_EVIDENCE.md) | Integrated | E |
| G | `cb8ba04` | [Structured messages and versioned persistence](../libraries/cog-core/contracts/session_store/CONTRACT.md#independent-acceptance-and-combined-gq-integration) | Integrated | E |
| H | `055211c` | [Provider codec and OpenRouter adapter](REAL_CLIENT_H_EVIDENCE.md) | Integrated | F,G |
| I | `03f60d7` | [Tool policy and workspace/process tools](REAL_CLIENT_I_EVIDENCE.md) | Integrated | E |
| J | `32db464` | [CLI config and credential resolution](REAL_CLIENT_J_EVIDENCE.md) | Integrated | E |
| K | `78760e5` | [Real conversation and bounded tool loop](REAL_CLIENT_K_EVIDENCE.md#main-integration) | Integrated | H,I,J,Q |
| L | `ff2dbc9` | Resume/fork and terminal interaction | In progress | G,K |
| M | `d0f910f` | Single-process coding-client acceptance | Pending | L, local-agent package `bf7a450` (complete) |
| N | `035219b` | Headless/subagent integration contract refinement | Pending | M |
| O | `0e35c23` | Headless reconnect and bounded subagents | Pending | N, replay primitive `91108c5` (complete) |
| P | `77adfec` | Release packaging and full validation | Pending | O |

Snapshot: 2026-11-15, integrated main `b8f28ad`. The combined real-library Q/G
check `b8a6fc1` is complete: strict and ASan/UBSan targeted suites each passed
27/27; see the [G/Q acceptance record](../libraries/cog-core/contracts/session_store/CONTRACT.md#independent-acceptance-and-combined-gq-integration).
Earlier evidence reports retain their dated scope and then-open dependencies.

Two primitive deliveries were pulled ahead of O and are complete:

- Local agents: `bf7a450`, legacy-test migration `b569276`, and
  [reviewed integration](REAL_CLIENT_LOCAL_AGENT_INTEGRATION.md) supply leased
  SPAWN/CANCEL, copied config, bounded updates and CGA1. O retains authenticated
  remote routing, placement and process lifecycle composition.
- Replay: `91108c5` and [CGR1 evidence](REAL_CLIENT_REPLAY_EVIDENCE.md) supply
  bounded retention, GAP and codecs. K supplies CGD1 display snapshots; O retains
  reconnect, snapshot/resync and deduplication integration.

Release assertion preservation `0966ef0` is also integrated: PRIVATE test-only
undefine flags, early explicit opt-in and executable-registration/forced-failure
checks are documented in the [test assertion policy](../cmake/tests/README.md).
This restores Release test workloads; it does not turn historical Release passes
into new runtime evidence. Process-fixture determinism follow-up `b4c9ca3` is
integrated; its [scoped evidence](REAL_CLIENT_PROCESS_FIXTURE_EVIDENCE.md) preserves
P full-profile validation as open. O migration `d5280b7` remains open.

## Scheduling and completion rules

A and B may run concurrently. C–E are serial header/scenario reviews. The initial public contracts and
compatibility changes for C are recorded in [REAL_CLIENT_CONTRACTS.md](REAL_CLIENT_CONTRACTS.md);
that pass alone does not satisfy D or E.
After E, F/G/I/J/Q may run concurrently only in isolated worktrees with
nonoverlapping file ownership. H waits for F/G; K joins H/I/J/Q; L completes
single-process UX. M is the first usable-client acceptance gate. N–P finish
the full headless/subagent release target. Shared public headers are edited
serially; conflicting implementations wait or are split.

Do not claim implementation-ready status for scaffold delivery tickets.
After E, refine each cohesive package into concrete source-file tasks plus
review, integration and finalize children. Preserve package dependencies.
Close a package only when its child work and evidence are complete.
A failed review returns work to its owner; repeated failures require design
reassessment per cog-dev. No user confirmation is needed for routine fixes.

The parent stays in progress until release gate P passes. A–K/Q and the local
agent/replay/assertion primitives are complete; L owns the next application UX
delivery. M acceptance, N/O headless integration and P release validation remain
outstanding. Credentials are accessible through the OpenRouter entry
in ~/.pi/agent/auth.json; no new key is presently required. Never include its
contents in logs, shell arguments, tickets, fixtures or source control.

## Verification plan

Accepted transport evidence and reproduction commands are in the
[HTTPS lifecycle report](research/https-lifecycle.md). A passes as research:
it does not approve libcurl allocation semantics or bounded DNS shutdown.
The accepted [DNS follow-up](../pocs/https-lifecycle/dns/README.md), child
`23ae6c8`, resolves backend selection: require a verified c-ares primary resolver
with threaded resolver disabled; reject the installed threaded and hybrid
profiles rather than relying only on ASYNCHDNS. The controlled c-ares local-DNS
cancel/timeout/cleanup suite passes without leftover threads or descriptors;
QUICK_EXIT leaves a thread alive after cleanup. Measured ceilings are empirical,
and maintained shipped profiles must rerun the probes. Passed gate E specifies
feature rejection/lifecycle and accepts explicit DYNAMIC third-party allocation
without a total byte ceiling. F records implementation OOM/cleanup and lifecycle
checks; P must revalidate the shipped dependency profile and installation. Q
records timer, IPC and cancellation progress with a nonempty core event queue;
the earlier PoC used only a synthetic queue.

Accepted protocol evidence is in the [JSON/SSE report](research/provider-wire.md):
yyjson 0.12.0 with explicit fixed pools and separate depth preflight, three
offline cases in Release and sanitizer builds, and two successful synthetic
live requests only (512 output tokens each; no retries). Saved fixtures prove
identified tool/result correlation and repeated finish metadata. Reasoning was
disabled; opaque continuation ownership/serialization/persistence and recoverable
delta capacity semantics have component coverage in the accepted
[H delivery](REAL_CLIENT_H_EVIDENCE.md) and G persistence. Final application and
release coverage remains gated. Research review is offline and
must not repeat the already completed live smoke.

Each package validates its contract and negative paths. Gates run strict
GCC/Clang builds, standalone headers, deterministic scenarios and appropriate
separate sanitizer builds. Live smoke is opt-in in normal CI, preapproved for
this autonomous project, synthetic and capped as defined in the blueprint.
Use public catalog metadata to revalidate the exact model before requests.
Do not require nondeterministic model wording; assert protocol completion,
tool/result associations and actual sandbox task outcomes.

M is single-process acceptance, not full release. It requires strict GCC/Clang
**full target builds**, exact recorded runtime selections, relevant separate
sanitizer/concurrency runs, a disposable read/edit/build/resume task and bounded
real application smoke. The current diagnosed O-dependent runtime exceptions are
`unit-orchestrator`, `integration-orchestrator-ipc`,
`integration-orchestrator-stress`, `scenario-test_07_aux_crash`,
`scenario-test_09_subagent_messaging` and `scenario-test_10_fan_out_fan_in`.
[K independent review](REAL_CLIENT_K_EVIDENCE.md#independent-critic-acceptance) records their
actual failures: fixtures assume synchronous connect OK instead of AGAIN, and
production orchestrator paths lack nonblocking endpoint service. Ticket
`d5280b7` owns runtime and fixture migration; accepting AGAIN in a helper alone
is insufficient. K's scenario03 and corrected scenario06 checks pass; original03
end-to-end remote behavior remains an O obligation.

For M, record the complete command and outcomes, including these failures when
still present, rather than calling an excluded selection a full-suite pass.
The exact prior K selection is preserved in its evidence; L's added checks must
also be assessed at M. P requires the **full suite without these exclusions**,
all 20 original plus R01–R18 executable scenario oracles and shutdown regression,
separate sanitizers, installed CLI restart and external embedder/dependency
closure. The completed read-only inventory `543f057` is an assertion/ownership
map, not a completed all-38 runtime gate; current names alone do not prove those
oracles. O exceptions and timing follow-ups must be resolved before P.

This status refresh validates ticket identities and local documentation links.
It adds no runtime evidence and closes no acceptance gate.


## Pass-2 ownership additions

[Stage D scenario/amendment register](REAL_CLIENT_SCENARIO_PASS2.md) supplies the
all-38 contract trace. Q includes the bounded nonblocking IPC endpoint profile
needed before K; O supplies its headless/replay integration. G includes exact
checkpoint/durable owner/OFD guard persistence, and N/O implement the declared
quiescent same-host handoff. K owns constructor-bounded visible snapshot state
and CGD1 codec; O owns CGR1 reconnect/GAP behavior. Core-local CGA1 primitives are
integrated; N/O own authenticated forwarding, CGH1 handoff and public orchestrator
service/admission. N may refine
integration but cannot defer an undeclared original-scenario requirement past E.
Any later header amendment repeats trace/review gates before code.
