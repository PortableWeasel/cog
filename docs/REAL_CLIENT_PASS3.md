# Stage 14.E: header pass 3

Ticket `feb17b6`; reviewed integrated pass 2 at `d4e3d7c` and the amended
headers in this pass. The architect's third pass is complete. **E remains in
progress for the additional full independent final review.** No production C,
CMake, runtime scenario, sanitizer result or paid provider request is delivered.

## Review outcome and allocation decision

All 38 required traces were reviewed against their declaring headers, including
normal flow, cancellation, overflow, OOM, shutdown, replay, migration and
source/ABI compatibility. Seven concrete contract gaps were corrected below;
affected traces include explicit pass-3 retraces. No known required public API
gap remains in the supported profile after these amendments. Independent final
review can reject that conclusion; implementation must not begin before E closes.

**Accept the declared DYNAMIC transport profile at the contract level.** Net and
request constructors visibly own CURLM/easy/header/dependency handles until
matching free/request_release. Curl/c-ares/TLS may allocate during service within
those explicit lifetimes; the host acknowledges this through DYNAMIC. This meets
visible ownership without asserting constructor-only allocation or a total byte
ceiling. A nonzero dependency_memory_limit is UNSUPPORTED, never ignored. The
72 observed polling allocations and curl-only measurements do not establish a
process memory cap or exhaustive dependency OOM proof. No Cog allocator global,
reference counter, lazy global init or destructor-triggered global cleanup is
permitted. The host owns process dependency initialization and cleanup, including
other library users. Cog-owned runtime workspaces stay reserved and bounded;
this decision does not authorize hidden Cog malloc/realloc during service.

Only verified c-ares-primary builds with threaded resolution disabled, recorded
build provenance and the runtime DNS_SERVERS/ares/ASYNCHDNS checks qualify.
Unknown/hybrid/threaded builds and QUICK_EXIT abandoned cleanup remain rejected.
Delivery F/P must repeat DNS, TLS, two-context cleanup and OOM tests on the
maintained security-patched shipped profile. Research timings are empirical,
not hard bounds on scheduler, TLS, filesystem or allocation latency.

## Amendments and affected contracts

| ID | Concrete failure found | Resolution | Delivery |
|---|---|---|---|
| E01 | A crashed STARTED call becomes immutable INDETERMINATE; normal guard refuses it, but save requires a guard. No explicit recovery could ever resume that session. | Recovery-only guard permits only monotonic reconciliation writes; borrow/append/checkpoint/effects refused. `reconcile_tool` atomically appends the identified result and closes the call using verified outcome or explicit ACKNOWLEDGED unknown effects. Preserve audit evidence; save, release, advance epoch, then acquire normal execution. | G/L/K/N |
| E02 | Core lacked admitted frame/aggregate payload ceilings and a consume-to-enqueue ownership rule. A 1 MiB endpoint frame could exceed its receive buffer or lose bytes under queue saturation. | Explicit core frame ceiling and constructor payload arena; reserve event+bytes before receive, backpressure without consuming, retain through handler, close/report only an over-limit peer. Accepted endpoint construction is explicit/count-bounded; intrusive listener linkage needs no new API. | Q/O |
| E03 | Tool parser/schema workspace and depth were promised bounded but had no public capacity or numeric/Unicode semantics; descendant cleanup could tempt process-global host changes. | Explicit argument/schema/depth/codec ceilings, strict duplicate/UTF-8 handling, finite numeric subset and scalar-count length rules. Supervision/subreaper changes remain in an owned helper; supported descendants remain in the owned process group. | I/K |
| E04 | Empty opaque array presence and unfamiliar members during text/summary merging could be lost. | Preserve explicit empty-array presence and union of non-payload members; repeated conflicting JSON values fail before effects. No speculative concatenation or dropping unknown metadata. | H/G |
| E05 | Successful pending-message enqueue could recycle backing text before its handler; explicit continuation after an incomplete turn was unspecified. | Lease injected text through handler return or copy into another reserved owner. Explicit continuation forks at the last safe ancestor and preserves the failed branch; queued prompts cannot choose it automatically. | Q/K/L |
| E06 | A custom store callback received a private wrapper pointer, with no supported caller state. | `backend_context` is caller-owned, passed verbatim, never freed by generic store code. Builtins retain constructor-owned contexts; required callbacks are validated. No private layout/global state needed. | G/P |
| E07 | Config retained one stale transaction handle per begin with no finite ceiling; set/commit/prefix copying had no constructor workspace contract. Concurrent-read wording contradicted its transactional API. | Bounded tree constructor with aggregate node/string/handle/validator pools; atomic LIMIT and bounded tombstones, no runtime growth. Every config call remains on its owner thread. | J |

E01 does not claim exactly-once execution or infer the effect from missing output.
An operator must explicitly authorize reconciliation after old processes are
fenced/reaped. ACKNOWLEDGED carries a result explaining that effects remain
unknown. Recovery cannot create new user/assistant/tool-call work, mutate prior
opaque/history bytes or grant normal execution. A failed recovery save retains
uncertainty. Interactive CLI may hold a recovery-only app; noninteractive mode
returns INDETERMINATE without effects. This closes recovery without an unsaved
execution bypass.

## Full scenario review

Each row names the principal edge checked. “Retained” means the existing contract
was assessed, not that its implementation or executable test passed.

| Trace | Principal boundary assessed | Pass-3 disposition |
|---|---|---|
| 01 | Request copy, retained delta, one assistant, full event queue | E02 |
| 02 | Stall vs transparent replay; finite timer-driven retry | Retained |
| 03 | Remote peer/epoch/sequence, copied notifications and cancel terminal | E02 |
| 04 | Explicit runtime/profile, constructor unwind and transfer-on-success | E02/E07 |
| 05 | Stop vs owned drain/reap/free and host cleanup | Retained |
| 06 | Effect once across capture/delivery overflow; explicit concurrency | E03 |
| 07 | Reaped/fenced crash, no task replay, explicit effect recovery | E01 |
| 08 | Prepare-before-commit and old generation lifetime | E07 |
| 09 | Partial framing, lifecycle ingress and borrowed event data | E02 |
| 10 | Partial fan-out admission and one correlated terminal per child | Retained |
| 11 | GAP during active text, session-bound atomic snapshot anchor | E02 |
| 12 | 429 metadata and retry cancellation at timer deadline | Retained |
| 13 | v1 inspection, v2 atomic publish and committed-prefix recovery | E01 |
| 14 | Immutable ancestry, reused call labels and branch-specific results | Retained |
| 15 | Complete arguments, durable STARTED and identified results | E03 |
| 16 | Sanitized stable errors and explicit partial failure | Retained |
| 17 | Bootstrap/adoption/delete/lock/CAS/route/restart ownership | E01 |
| 18 | Static sinks, bounded formats and borrowed diagnostics | Retained |
| 19 | Transport vs HTTP outcomes and copied connection events | Retained |
| 20 | Cancelled aggregate, pending FIFO lifetime and explicit continuation | E05 |
| R01 | Presence/defaults, credential cleanup and durable REAL startup | E07 |
| R02 | One wire turn, copied history and sustained-queue service | Retained |
| R03 | Split identity, typed expansion and exact opaque assembly | E04 |
| R04 | Save/effect/result crash boundaries and output-resize retries | E01/E03 |
| R05 | No-store/ASK/invalid schema makes zero executions | E03 |
| R06 | One attempt+elapsed retry budget across queued work | Retained |
| R07 | Partial output forbids replay, explicit safe continuation branch | E05 |
| R08 | DNS/connect/backoff cancel with one retained terminal | E05 |
| R09 | Process group, capture saturation, reap and effect uncertainty | E01/E03 |
| R10 | Generation exactness, corrupted prefix, owned deletion and recovery | E01 |
| R11 | Exact opaque identity across fork and namespace refusal | E04 |
| R12 | Old config/provider retained while new generation publishes | E04/E07 |
| R13 | Empty-window GAP, cross-session snapshot and slow peer | E02 |
| R14 | Remote update backpressure, cancel race and fenced recovery | E01 |
| R15 | Byte/depth/parser limits and transactional invalid input | E03 |
| R16 | Drain borrowers before guard/store/host teardown | E01 |
| R17 | Finite output/context/round/time bounds and known usage | E03 |
| R18 | Public provider construction, independent net contexts and link closure | E06 |

Replay CGR1 validators distinguish DATA, GAP and CURSOR and define empty windows.
CGD1 carries session identity and samples complete/active visible state with its
cursor atomically. Encoding happens before a mutable app call invalidates scratch;
send OK copies once and AGAIN accepts nothing. Destination capacity failures
publish no partial metadata, display or cursor. Replay never authorizes effects.

Owned migration is same-host/shared-store/quiescent only. Source freezes, drains
and checkpoints under its guard; target loads the exact immutable generation.
CAS takes the same independent-open-description lock, compares owner+generation
and durably changes epoch. Target remains staged through normal-guard acquisition
and initial advisory route publication. Stale ACKs/results cannot restore a past
owner. Abort/resume advances epoch. Bootstrap permits absent owner/data; adoption
requires exact replay-safe unowned v2 after old writers quiesce. All new-profile
writes serialize under the same ownership domain. Owned deletion/anchor replacement
is UNSUPPORTED even after release, preventing inode/fencing reset. Uncertain fsync
requires exact locked durable confirmation, never guessed rollback.

## API, include and delivery migration

The public include graph is acyclic: result is the shared value root; IPC/obs do
not include core state; core includes their contracts and forward-declares provider.
Provider depends on core message contracts, and OpenRouter composition additionally
includes net. Toolkit depends on core, including a forward-declared guard; orchestrator
includes core store/IPC contracts. No core link edge to provider/net/toolkit is added.

Delivery must update exported targets: provider publicly propagates net usage
requirements; CLI propagates toolkit and net; orchestrator retains core+IPC.
Private yyjson pools stay in provider/toolkit. Backend dependency link closure
(including curl/c-ares/TLS and static transitive requirements) is checked by actual
installed-consumer tests in F/H/P. The current older CMake and source bodies are
not evidence of the new contract; they intentionally await gated migration.

This is a pre-alpha source/ABI revision. Positional vtable/config/message/tool
initializers, custom store callback contexts, provider construction, nonblocking
IPC service, net peek/ack/release, store generation/owner/recovery and new result
codes need explicit delivery changes. New fields default as documented, not through
uninitialized trailing bytes. Custom fenced stores supply recovery acquisition;
missing capabilities fail construction. No existing binary compatibility is claimed.

## Evidence and remaining implementation obligations

The final matrix compiles all **28 public headers** under strict GCC C11, G++17,
Clang C11 and Clang++17 (**112 standalone objects**), plus a mixed consumer under
each compiler that references every declared symbol/member in all 38 trace tables.
The audit checks exact declaring-header references, typed fields by compiler,
local links and an acyclic public include graph. The final audit covers **916 table references, 109 distinct typed fields and
74 public include edges** after all amendments. Synthetic fixture identity assembly and exact opaque
UTF-8 length/base64 equality pass; new data-only oracles cover recovery, unknown
opaque members and constructor limits. Whitespace and no-production-C diffs pass.

Those checks supplement semantic tracing; they are not executable conformance.
G must prove monotonic recovery diffs, guard modes, exact generation and all
crash/fsync/delete races; Q must prove arena/FIFO lifetimes and service fairness;
I must prove one effect across every overflow/cancel/reap boundary; J must prove
bounded config/reset limits; H must prove strict parser/opaque fixtures. All old
scenario executables must be migrated and pass before their delivery gates.

No known required declaration gap is left open in this pass. The additional full
independent final review must assess these new recovery/bounds decisions before
closing E. No production implementation is authorized by this report alone.
