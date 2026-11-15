# Agent lifecycle contract

The public contract is `include/cog/agent.h`. A core reserves record and
message storage at construction (defaults: 16 records and 65536 bytes per
field). Spawn owns a child session and copied configuration; its explicit
session constructor is the allocation boundary. Exhausting the configured
record or string capacity returns `COG_ERROR_LIMIT` without publishing an ID.
IDs increase for the lifetime of a core, even when a released slot is reused.
Spawn and cancellation each enqueue a core-owned intent atomically; queue-full
returns OVERFLOW without publishing an ID or admitting cancellation. Intent
payload leases last through dispatch, including error returns. Release refuses
queued or in-dispatch leases; handlers copy intent and configuration to caller
storage before deferring work. Public enqueue rejects forged intent kinds.

A spawned attempt has an immutable owner peer and execution epoch. A remote
owner supplies a nonzero epoch; a zero local epoch selects the assigned agent
ID. The application authenticates a transport envelope before invoking
`cog_agent_apply_update`. Updates have contiguous nonzero sequence numbers;
identical latest retries are idempotent and conflicting duplicates are
protocol errors. One copied notification remains pending until acknowledged.
A distinct successor returns `AGAIN` without changing state while that
notification is pending. `copy_update` copies into independent caller storage;
an overflow changes only its required length. Ack retains the snapshot and
terminal result for later reads.

Cancellation admits a request and rejects subsequent RUNNING or COMPLETED
updates for that attempt; a correlated drained/reaped CANCELLED, TIMEOUT, or
fenced FAILED update can establish the terminal state. A terminal record is
immutable. Terminal results, including failure outcomes, are readable. Release
requires a terminal record with its notification acknowledged; it frees the
owned child session and makes the slot reusable. Copied notification data is
independent of the record and survives release and core destruction.

CGA1 encoding and decoding use fixed-width big-endian fields, strict UTF-8,
explicit lengths, and caller storage. Invalid versions, codes and frames fail
closed; an undersized output reports the needed size without partial output.
The core-local lifecycle does not authenticate remote peers, send IPC frames,
place processes or run an agent loop; Stage 14.O owns those integrations.

The private `cog_agent_internal_*` hooks are legacy test seams that delegate
to public update validation and retain notifications; they never auto-ack.
New integration uses the public update/copy/ack protocol. All lifecycle operations are owner-thread only; no update, copy,
acknowledgement, result or release allocates memory.

The allocator regression in `tests/unit/test_agent.c` is enabled with
`-DCOG_TEST_WRAP_ALLOC` and linker options
`-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc`; it tracks update, copy, ack,
result, cancel, intent dispatch and release after explicit spawn construction.
