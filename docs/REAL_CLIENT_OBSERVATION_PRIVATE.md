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
