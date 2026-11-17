# Stage 14 execution roadmap

> [Stage 14.E final review](REAL_CLIENT_PASS3.md) passed after the additional
> independent review of `8563f81`. The three header passes and final review are
> complete. A–L/Q, R12 and runtime observation are integrated. M is accepted;
> N's integration contracts passed. O's component, O9 and installed deliveries
> are integrated at `2723c36`; whole-O review and P remain open.

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
| L | `ff2dbc9` | [Resume/fork and terminal interaction](REAL_CLIENT_L_INTEGRATION_EVIDENCE.md) | Integrated | G,K |
| M | `d0f910f` | [Single-process offline acceptance](REAL_CLIENT_M_OFFLINE_EVIDENCE.md) and [bounded installed live smoke](REAL_CLIENT_M_LIVE_EVIDENCE.md) | Accepted, dated single-process scope | L and required followups |
| N | `035219b` | [Headless/subagent three-pass integration contracts](REAL_CLIENT_HEADLESS_PASSES.md) | Passed | M |
| O | `0e35c23` | [O9 actual joins](REAL_CLIENT_O9_INTEGRATION_EVIDENCE.md) and [installed public proof](REAL_CLIENT_INSTALLED_PROOF_INTEGRATION.md) | Deliveries integrated; whole-O review `5cd1d25` open | N and component gates |
| P | `77adfec` | Release packaging and full supported-profile validation | Open | Accepted whole O |

Current integration snapshot: `2723c36`, before whole-O/P approval or a main
merge. The combined real-library Q/G
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
P full-profile validation as open. Orchestrator migration `d5280b7` is complete;
the accepted O3/O4/O9 deliveries replace its old synchronous-fixture assumptions.
R12 reload and [real observation](REAL_CLIENT_OBSERVATION_INTEGRATION_EVIDENCE.md)
are integrated. The [release assertion map](REAL_CLIENT_RELEASE_COVERAGE.md)
connects every original20/05b and R01–R18 requirement to actual oracles and scope.

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

The parent stays in progress until release gate P passes. M acceptance is
complete at its recorded single-process scope; the six former M exceptions
are resolved by integrated O deliveries, not retroactively removed from M's
dated report. Whole-O package review and P still require their own exact source,
profile, inventory and result receipts. Routine documentation work neither
repeats the paid smoke nor closes those gates.

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
release coverage is recorded separately. Research review is offline and
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
real application smoke. [M's dated report](REAL_CLIENT_M_OFFLINE_EVIDENCE.md)
preserves its six actual O-dependent exceptions and129 applicable-case result.
The [O9 integration](REAL_CLIENT_O9_INTEGRATION_EVIDENCE.md) later passed184/184;
the [installed extension](REAL_CLIENT_INSTALLED_PROOF_INTEGRATION.md) passed
185/185 in the admitted GCC Release profile, with no exclusions or disabled
cases. Those results establish their exact profile, not fresh full Clang/ASan/
TSan coverage. Whole-O review `5cd1d25` and P retain that work.

P requires the **full suite without the former M exclusions**,
all 20 original plus R01–R18 executable scenario oracles and shutdown regression,
separate sanitizers, installed CLI restart and external embedder/dependency
closure. The completed read-only inventory `543f057` is an assertion/ownership
map, not a completed all-38 runtime gate; current names alone do not prove those
oracles. The current map names actual runtime assertions, including independent
STARTED/RUNNING readiness facts, crash uncertainty, same-host transfer limits
and preserved component controls.

This documentation refresh adds no runtime evidence and closes no gate.


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
