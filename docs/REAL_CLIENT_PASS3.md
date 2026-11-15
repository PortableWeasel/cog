# Stage 14.E: header pass 3

Ticket `feb17b6`; reviewed integrated pass 2 at `d4e3d7c` and the amended
headers in this pass. The architect's third pass and the **additional full
independent final review passed on revised tip `8563f81`**, after one correction
cycle. Gate E is accepted for the declared supported profile. No production C,
CMake, runtime scenario, sanitizer result or paid provider request is delivered.

## Review outcome and allocation decision

All 38 required traces were reviewed against their declaring headers, including
normal flow, cancellation, overflow, OOM, shutdown, replay, migration and
source/ABI compatibility. Eleven contract gap groups, including the independent-review revision below, were corrected;
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

## Independent final-review revision 1

Final reviewer rejected four additional boundary groups; this revision is sent
back for review and is **not** gate approval. Provider disconnect now explicitly
consumes its handle even on cleanup error, and async tool construction validates
all six callbacks before calling implementation code.

- **E08, monotonic committed identity:** reset/discard cannot erase identified,
  admitted, result/audit-bearing or fork-shared state. Draft text disposal remains
  explicit, and counters never rewind. Normal save/append now checks the locked
  committed generation; replacing a session object with the same ID cannot erase
  or rebind a call, its assistant/ancestor context, results, audit or high-water.
  Only legal monotonic transitions are allowed, and final outcomes stay immutable.
  New INDETERMINATE halts admission under an already held normal guard. G/Q/K/L
  must test this through both public mutation and replacement-object saves.
- **E09, reachable recovery:** unresolved calls/results stay on one active path.
  Fork or arbitrary-parent insertion cannot strand an uncertain sibling. Text-only
  incomplete continuation remains supported. G/K/L/N test sibling-switch refusal
  and reachable reconciliation after a crash.
- **E10, public exact restore:** custom backends now export a bounded snapshot
  view and use an explicit restore constructor for sparse IDs, active leaf,
  identity high-water and complete ledger/audit records. No private layout or
  dummy IDs. Restore permits larger constructor reservations so a valid full
  snapshot can gain recovery-result space without in-place growth. G/P must test
  public-only custom save/load, malformed identity/audit and capacity retries.

- **E11, proven-unstarted eligibility:** coherent PROPOSED does not require
  recovery: normal acquisition resumes only that current explicit policy/denial,
  while crashed STARTED becomes INDETERMINATE. Cancel closes unstarted proposals
  with identified no-effect denial. G/K/L test pre-STARTED crashes with zero effects.
  No-store remains supported text-only; unsolicited calls are denied without a
  durable-STARTED bypass, and safe text draft disposal remains available.

Affected traces and the synthetic fixture carry these failure oracles. The final
independent reviewer must recheck the revised normal-save/recovery/restore bounds.

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
local links and an acyclic public include graph. The final audit covers **940 table references, 112 distinct typed fields and
74 public include edges** after all amendments. Four additional strict C/C++
custom-store consumers compile the public backend-context/snapshot/restore path
without private headers (eight mixed/custom consumer objects total). Synthetic fixture identity assembly and exact opaque
UTF-8 length/base64 equality pass; new data-only oracles cover recovery, unknown
opaque members and constructor limits. Whitespace and no-production-C diffs pass.

Those checks supplement semantic tracing; they are not executable conformance.
G must prove monotonic recovery diffs, guard modes, exact generation and all
crash/fsync/delete races; Q must prove arena/FIFO lifetimes and service fairness;
I must prove one effect across every overflow/cancel/reap boundary; J must prove
bounded config/reset limits; H must prove strict parser/opaque fixtures. All old
scenario executables must be migrated and pass before their delivery gates.

No known required declaration gap is left open after the independent final
acceptance recorded below. Runtime implementation must follow the approved
contracts and its separate review/integration gates.


## Independent final acceptance and integration

The independent critic reviewed the complete accepted design/research, all 38
traces and public contracts, then rejected the initial tip `e71734d` in revision
1 for E08–E11. The revised tip `8563f81` closes those findings: committed effect
identity cannot be erased or rebound; unresolved calls remain recoverable on one
active path; public snapshot/restore preserves exact identity and audit with
explicit larger reservations; proven-unstarted PROPOSED remains distinct from
uncertain effects. Recovery cannot execute tools or silently erase uncertainty.

Final independent validation passed 120 strict compiler objects: all 28 public
headers under GCC/G++/Clang/Clang++, four complete trace-symbol/member consumers,
and four separately authored public custom snapshot/restore consumers. All 940
exact table references, 112 distinct typed fields, 74 actual acyclic include
edges, local links and exact synthetic opaque UTF-8/base64/length checks passed.
The reviewed tip was preserved in merge `072de88`; integration ticket `6dca00c`
records the clean merge and final status checks. No production C or CMake changed.

The independent review accepts the declared DYNAMIC dependency profile as visible
handle ownership with explicit host acknowledgement, never a total-memory or
constructor-only claim. Cog runtime workspaces remain bounded; global Cog
allocator hooks, refcounts and implicit dependency initialization/cleanup remain
forbidden. Maintained shipped-profile and runtime conformance tests remain
required. Header gate E is complete; F/G/I/J/Q may be refined into implementation,
review and integration tasks. This acceptance does not mark any delivery or
release runtime gate complete.
