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
