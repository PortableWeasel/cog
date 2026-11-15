# Stage 14 execution scaffold

Parent: `2b9d0ea`. This is the current real-client roadmap; Stage 12 remains
historical engine/mock-CLI delivery evidence. User delegates routine design
and preapproves autonomous testing. No pending scope-confirmation gate.

## Dependency-ordered work

The roadmap contains 17 packages (A–Q); Q is placed beside its prerequisite
header gate because it must complete before K.

| Package | Ticket | Deliverable | Prerequisites |
|---|---|---|---|
| A | `c5671b5` | Research: HTTPS lifecycle and event-loop PoC (integrated) | — |
| B | `2e1cddf` | Research: JSON/SSE and OpenRouter wire fixtures | — |
| C | `5df3ba1` | Design: public headers pass 1 | A,B |
| D | `345a536` | Design: scenario pass 2 and gap amendments | C |
| E | `feb17b6` | GATE: header pass 3 and independent final review | D |
| Q | `bcd9c0a` | Delivery: fair core service scheduling | E |
| F | `8f55808` | Delivery: transport HTTPS and polling | E |
| G | `cb8ba04` | Delivery: structured messages and versioned persistence | E |
| H | `055211c` | Delivery: provider codec and OpenRouter adapter | F,G |
| I | `03f60d7` | Delivery: tool policy and workspace/process tools | E |
| J | `32db464` | Delivery: CLI config and credential resolution | E |
| K | `78760e5` | Delivery: real conversation and bounded tool loop | H,I,J,Q |
| L | `ff2dbc9` | Delivery: resume/fork and terminal interaction | G,K |
| M | `d0f910f` | GATE: single-process coding client | L |
| N | `035219b` | Design: headless/subagent integration contracts | M |
| O | `0e35c23` | Delivery: headless reconnect and bounded subagents | N |
| P | `77adfec` | GATE: release packaging and full validation | O |

## Scheduling and completion rules

A and B may run concurrently. C–E are serial header/scenario reviews.
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

The parent stays in progress until release gate P passes. The planning
baseline and independently reviewed transport research A are integrated;
JSON/SSE research B, actual header passes, implementation and release
validation remain outstanding. Credentials are accessible through the OpenRouter entry
in ~/.pi/agent/auth.json; no new key is presently required. Never include its
contents in logs, shell arguments, tickets, fixtures or source control.

## Verification plan

Accepted transport evidence and reproduction commands are in the
[HTTPS lifecycle report](research/https-lifecycle.md). A passes as research:
it does not approve libcurl allocation semantics or bounded DNS shutdown.
The installed threaded resolver is excluded from bounded cancellation after
a controlled stall blocked removal for about 1.9 seconds; the reproducible
follow-up and alternative resolver investigation remain pending. Header gate
E must resolve DNS backend/latency and third-party allocation/OOM/memory-limit
contracts before delivery adoption. Q must prove timer, IPC and cancellation
progress while the core event queue remains nonempty; the PoC only uses a
synthetic queue.

Each package validates its contract and negative paths. Gates run strict
GCC/Clang builds, standalone headers, deterministic scenarios and appropriate
separate sanitizer builds. Live smoke is opt-in in normal CI, preapproved for
this autonomous project, synthetic and capped as defined in the blueprint.
Use public catalog metadata to revalidate the exact model before requests.
Do not require nondeterministic model wording; assert protocol completion,
tool/result associations and actual sandbox task outcomes.

Planning validation checks ticket identities, parent/dependency edges,
acyclic ordering and local documentation links. It does not claim any new
implementation test coverage or a completed header gate.
