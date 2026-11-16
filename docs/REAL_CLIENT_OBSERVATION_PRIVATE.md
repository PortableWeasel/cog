# Application observation private interfaces

Parent `b64ee7c`, private-interface child `ba4ec54`; baseline `b0da8a6`.
The approved public design is REAL_CLIENT_OBSERVABILITY_CONTRACTS.md at7096360.
No public API expansion or net implementation change is proposed.

## Pass 1

`observation.h` declares an explicit host-owned constructor/destructor, copied
scope, bounded numeric callback, owner drain/status/domain clear and app timing
checkpoints. Main reserves4 rows and18 queue entries before creating its net with
max_connections4, then binds app obs privately after app_new. Embedding apps keep
their external observer; they still check its net status and their typed timing
errors. Bridge memory outlives net callbacks, while copied labels outlive obs
diagnostics until rows clear before target destruction.

Private configuration labels are cached during each startup/reload constructor
after secret resolution/copy. Only safe labels leave that owner. Explicit
session/source IDs can be cached; auto-generated session IDs are unavailable,
which is represented explicitly rather than guessed or dynamically inspected.
All effective values and secret/resolver return semantics remain unchanged.

The app gains only private timer/span/error/scope and optional borrowed bridge
state. Existing result APIs remain the sole primary channel. Observation errors
gate later admission; ordinary cancellation/reap/durable cleanup still completes.
Provider/tool static spans and typed duration records have no raw payload fields.

## Pass 2: fifteen concrete traces before bodies

| Approved trace | Private call/lifetime sequence |
|---|---|
| 1 success | prepare copied scope; provider_send; drain OPEN; every poll/next/cancel/disconnect drains numeric events outside net callback |
| 2 failed send | begin checked timer; failed send; end failed attempt once; no invented OPEN or diagnostic row |
| 3 admitted retry | old provider FINISH ends old span; budget grants retry flag; actual successful send queues OPEN; drain emits OPEN then RETRY at the same callback timestamp |
| 4 transport failure | net numeric enum/status only; copied scope supplies static endpoint/provider; no arbitrary peer error text |
| 5 stall | preserve queued callback time for STALLED/FAILED and last-progress snapshot; no guessed stall or connection |
| 6 callback/sink failure | callback latch can reach net status; later obs sink latch remains private and both freeze new admission; primary work error wins |
| 7 pause | telemetry never calls body selectors; trust validated source totals, verify sequence/delta continuity without counting callback retries |
| 8 active cancel/release | owner drains after cancel and after consumed disconnect, before freeing config labels or obs |
| 9 captured completion | record actual transport terminal separately from later provider semantic finish; no fabricated cancellation replacement |
| 10 driver failure | existing owner cancel/drain/reap continues despite observation failure; no fresh effect or record retry |
| 11 contexts | row key is net pointer plus request ID; monotonically assigned obs ID is separate; clear domain before pointer reuse |
| 12 reload | check/drain before reload and after each constructor/preflight/destructor; old row contains copied labels and scope, not configuration pointers |
| 13 tools | checked timer/span before start, end through joined reap; explicit no-execution record for refusal; preserve durable save/result even on observation error |
| 14 availability | cache screened labels during startup/reload; known cumulative usage replaces, unknown fields omitted; separate usage record keeps formatting bounded |
| 15 teardown | release all provider IDs, drain and clear rows, unbind obs, destroy obs, then host net_free and finally bridge_free |

This pass separates nonreused observational attempt IDs from core request serials,
so refused sends cannot reuse an observation identity or change stale-event rules.
It adds tool-terminal once state for retained failed-insertion handles. An OPEN
before binding is invalid; supported construction never sends before binding.
EOF/cancel/recovery and R12 fixed resource semantics are unchanged.

## Pass 3: consistency and error-domain review

No callback invokes formatting, obs, stdio or net APIs; capture reads the
nonblocking checked obs monotonic clock and copies fixed numeric/scope values.
Bridge status and net observer_status are separate; deferred sink failure cannot
be injected into net status and gates admission immediately without a further
callback. Cleanup drains owned effects even if the feed is already incomplete.
Terminal rows clear borrowed diagnostics before reuse; domain clear requires
zero owned requests, including retained terminal slots. Net dies before host
bridge memory, and obs detaches only after releases/drain/diagnostic clear.

Provider/tool static span names carry no external content. Typed metadata and
known usage use separate bounded records; unavailable labels/numbers are omitted
with availability flags. Private configuration code alone compares labels with
its credential bytes/reference; bridge scope receives only copied safe labels.
No raw secret accessor, reference/URL/argument/output/body/error-text log or
retired-generation borrow is introduced. Explicit generated-session omission
does not change the actual session ID/history or make valid configuration fail.

Observation identity is nonreused and independent of core request serial,
configuration identity, durable owner epoch and display incarnation. Failed sends
burn observation identities but create no fake net row. RETRY requires an actual
OPEN from the newly admitted application retry and keeps its timestamp. The
existing application budget remains the sole retry/effect authority.

Required pointers and constructors have explicit errors/destructors. Existing
R12 publication retains its infallible postcommit section; observation checks
precede commit/alias retirement, while every consuming disconnect is drained
before matching config storage clears. Final app_run result preserves work
failure first, with observation failure returned only if work otherwise succeeds.
No public declaration or net/core/provider implementation changes are needed.

## Independent source cycle 1: retained IDs and final target borrow

The first source review identified that net active_count excludes terminal
retention and that a raw observer AGAIN can be misread by core service as normal
no-progress. Reusable domain clear now checks request_status for every known key;
terminal rows cannot be reused until release. Owners must still explicitly release
all IDs, including unobserved IDs after feed loss, since the public API cannot
enumerate all retained identities. Final target detachment is a distinct private
operation: invalidate the feed explicitly, clear diagnostic borrows, NULL the
target and retain numeric keys while host net remains alive. It neither claims a
reusable context nor cancels unrelated requests, and allows app_free to consume
its app/obs resources without dangling pointers or leaks. App drive normalizes
AGAIN to IO while raw net/bridge error remains unchanged. Both corrections use
existing public primitives; no public contract or interface is extended.
