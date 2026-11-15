# event.h Contract

`libraries/cog-core/include/cog/event.h` — event kinds, the event
payload structs, and the bounded FIFO event queue that feeds a core
instance.

This document records the executable contract verified by
`tests/unit/test_event.c` and implemented by
`libraries/cog-core/src/event.c`. It intentionally does **not** expand
the public API; it only documents what the header already declares.

## Status

**Implemented** (`libraries/cog-core/src/event.c`). The queue is a
single-producer / single-consumer bounded ring buffer. All storage —
queue header plus the ring of event slots — is allocated in **one**
heap block by `cog_event_queue_new` and released by
`cog_event_queue_free`.
Guard: `COG_EVENT_H`. Includes `<cog/result.h>`, `<cog/ipc.h>`,
`<stdbool.h>`, `<stddef.h>`, `<stdint.h>`.

## Public types

| Type | Kind | Notes |
|------|------|-------|
| `cog_state_t` | `typedef uint16_t` | State identifier for the dispatch table (defined here; used by state.h). |
| `cog_event_id_t` | `typedef uint16_t` | Event identifier for the dispatch table. No ids are reserved. |
| `cog_event_kind_t` | `enum cog_event_kind` | Semantic kind of an event (26 values, 0..25). |
| `cog_event_t` | `struct cog_event` | Event passed through the state machine. |
| `cog_event_ipc_t` | `struct cog_event_ipc` | Payload for `COG_EVENT_IPC_*` events. |
| `cog_event_queue_t` | `struct cog_event_queue` (opaque) | Bounded FIFO event queue; definition lives in `src/event.c`. |

## Constants (event kinds)

Declared order and values (pinned by `tests/unit/test_event.c`):

| Constant | Value | Meaning |
|----------|-------|---------|
| `COG_EVENT_INVALID` | `0` | Uninitialised or sentinel value. |
| `COG_EVENT_USER_INPUT` | `1` | Free-form user message. |
| `COG_EVENT_TOOL_RESULT` | `2` | Result returned from a tool invocation. |
| `COG_EVENT_SYSTEM_SIGNAL` | `3` | Signal or notification from the runtime. |
| `COG_EVENT_UI_MESSAGE` | `4` | Message received from an attached UI process. |
| `COG_EVENT_SUBAGENT_RESULT` | `5` | Result returned from a subagent handler. |
| `COG_EVENT_SUBAGENT_STATUS` | `6` | Subagent progress/status notification. |
| `COG_EVENT_LLM_CHUNK` | `7` | Streaming chunk from LLM provider. |
| `COG_EVENT_LLM_COMPLETE` | `8` | LLM response finished. |
| `COG_EVENT_PROVIDER_ERROR` | `9` | Provider call failed (network, auth, etc). |
| `COG_EVENT_NETWORK_STALL` | `10` | Network connection stalled. |
| `COG_EVENT_NETWORK_RECOVERED` | `11` | Network connection recovered. |
| `COG_EVENT_STATUS_UPDATE` | `12` | Status update for UI display. |
| `COG_EVENT_IPC_MESSAGE` | `13` | Framed message received from a peer. |
| `COG_EVENT_IPC_CONNECTED` | `14` | New peer connected to the IPC fabric. |
| `COG_EVENT_IPC_DISCONNECTED` | `15` | Peer disconnected from the IPC fabric. |
| `COG_EVENT_CONFIG_CHANGED` | `16` | Configuration was reloaded. |
| `COG_EVENT_SUBAGENT_TIMEOUT` | `17` | Subagent exceeded its time limit. |
| `COG_EVENT_SHUTDOWN` | `18` | Request for graceful shutdown. |
| `COG_EVENT_CANCEL` | `19` | User requests cancellation of in-flight work. |
| `COG_EVENT_CANCELLED` | `20` | Cancellation completed successfully. |
| `COG_EVENT_QUEUE_MSG` | `21` | User queues a message for next turn. |
| `COG_EVENT_MSG_QUEUED` | `22` | Queued message acknowledgement. |
| `COG_EVENT_RETRY` | `23` | User requests retry of failed operation. |
| `COG_EVENT_ABORT` | `24` | User requests abort of failed operation. |
| `COG_EVENT_ERROR` | `25` | Error occurred (provider, tool, etc). |

## Struct layout

```c
typedef struct cog_event {
    cog_event_id_t id;       /* Dispatch id; often maps one-to-one to kind. */
    cog_event_kind_t kind;   /* Semantic kind of the event. */
    const void *payload;     /* Optional payload; may be NULL. */
    size_t payload_len;      /* Payload size in bytes; zero when payload is NULL. */
    uint64_t source_id;      /* Diagnostic source identifier; zero when unknown. */
} cog_event_t;

typedef struct cog_event_ipc {
    cog_peer_id_t peer;       /* Source or destination peer. */
    uint16_t msg_type;        /* Application message type tag. */
    const void *msg_payload;  /* Message payload (borrowed). */
    size_t msg_payload_len;   /* Payload length in bytes. */
} cog_event_ipc_t;
```

## Functions

| Function | Signature | Errors |
|----------|-----------|--------|
| `cog_event_queue_new` | `cog_result_t (cog_event_queue_t **out, size_t capacity)` | `COG_ERROR_NULL_PTR` (`out` NULL); `COG_ERROR_INVALID_ARG` (`capacity == 0`); `COG_ERROR_NOMEM` (allocation fails or the single-block size computation would wrap) |
| `cog_event_queue_free` | `void (cog_event_queue_t *q)` | none; `q == NULL` is a no-op |
| `cog_event_queue_push` | `cog_result_t (cog_event_queue_t *q, const cog_event_t *event)` | `COG_ERROR_NULL_PTR` (`q` or `event` NULL); `COG_ERROR_OVERFLOW` (queue full) |
| `cog_event_queue_pop` | `cog_result_t (cog_event_queue_t *q, cog_event_t *out)` | `COG_ERROR_NULL_PTR` (`q` or `out` NULL); `COG_ERROR_AGAIN` (queue empty) |
| `cog_event_queue_count` | `size_t (const cog_event_queue_t *q)` | none; returns 0 for NULL `q` |
| `cog_event_queue_is_empty` | `bool (const cog_event_queue_t *q)` | none; returns true for NULL `q` |
| `cog_event_queue_is_full` | `bool (const cog_event_queue_t *q)` | none; returns false for NULL `q` |

## Ownership and payload semantics

- **The queue owns the event structs it stores**, not the payloads.
  `push` copies the `cog_event_t` by value into queue storage: `id`,
  `kind`, `payload` (the pointer), `payload_len`, and `source_id` are
  snapshotted at push time. Mutating the producer's struct after `push`
  has no effect on the queued event.
- **Payload pointee is referenced, not owned** — with one documented
  exception. For every kind except `COG_EVENT_IPC_*`, the queue never
  dereferences or copies the payload bytes; the caller must ensure the
  payload outlives the event. Mutations of the payload buffer after
  `push` are visible in the popped event.
- **`COG_EVENT_IPC_*` payloads are snapshotted.** The header documents
  that "the event queue copies this struct", so for
  `COG_EVENT_IPC_MESSAGE`, `COG_EVENT_IPC_CONNECTED`, and
  `COG_EVENT_IPC_DISCONNECTED`, `push` copies the embedded
  `cog_event_ipc_t` struct (the `payload` pointee) into queue-owned
  per-slot storage and the popped event's `payload` points at that
  snapshot. The snapshot remains valid until the slot is overwritten by a
  later `push` or the queue is destroyed, so it is safe to read during
  handler execution. Producers of IPC events must supply
  `payload_len == sizeof(cog_event_ipc_t)`. If an IPC-kind event has a
  NULL `payload`, it is stored as-is (payload stays NULL; no snapshot is
  attempted).
- **`cog_event_ipc_t::msg_payload` is always borrowed.** The inner
  message bytes are owned by the IPC layer and are never copied; the
  snapshot copies only the struct fields.
- **Single heap block.** `new` performs exactly one `malloc` of
  `offsetof(header) + capacity * slot_size`; `free` performs exactly one
  `free` of the queue pointer. There is no per-element allocation.
- **Capacity is bounded** at construction: `capacity > 0` events max.
  There is no default capacity; the caller always specifies it.

## Error semantics

- `new`: NULL `out` → `COG_ERROR_NULL_PTR`; zero capacity →
  `COG_ERROR_INVALID_ARG`; either allocation failure or a size-wrap
  (capacity so large the single-block byte count overflows `size_t`) →
  `COG_ERROR_NOMEM`. On every failure `*out` is set to NULL and no
  resources are held.
- `push`: NULL `q` or `event` → `COG_ERROR_NULL_PTR`; queue full →
  `COG_ERROR_OVERFLOW`. On failure the queue contents and `count` are
  unchanged. On success the queue contains a copy of `event`.
- `pop`: NULL `q` or `out` → `COG_ERROR_NULL_PTR`; queue empty →
  `COG_ERROR_AGAIN`. On failure `*out` is unchanged. On success `*out`
  holds the oldest queued event and the queue advances.
- `count`/`is_empty`/`is_full` never fail; NULL `q` degrades to zero /
  empty / not-full respectively.

## Invariants

- `count` is always in `[0, capacity]`; `capacity > 0` is an invariant of
  the constructor.
- FIFO: `pop` returns events in exactly the order `push` accepted them,
  including across fill/overflow/drain boundaries and ring wrap-around.
- The tail index is `(head + count) % capacity`; a full queue never
  overwrites: `push` returns `COG_ERROR_OVERFLOW` while `count ==
  capacity`, so stored events are never clobbered by a rejected push.
- A zero-initialized `cog_event_t` (`(cog_event_t){0}`) is a valid event:
  kind `COG_EVENT_INVALID`, NULL payload, `payload_len` 0, `source_id` 0.
- The queue performs no payload validation beyond the IPC snapshot rule:
  it does not interpret `kind`, `payload_len`, or payload contents.
- `cog_event_ipc_kinds` is a `static const` lookup table in the
  implementation (indexer-friendly, no dynamic registration), matching
  the project's dispatch-table convention.

## Threading

Per header: single producer / single consumer. `push` and `pop` must not
be called concurrently on the same queue, and no other thread may access
a queue during or after destruction. `count`/`is_empty`/`is_full` are
reliable only from the single consumer (per header). No locking is
performed by the implementation.

## Verification

`tests/unit/test_event.c` pins (at compile time) the kind sentinel value
`COG_EVENT_INVALID == 0`, the exact field types of `cog_event_t` and
`cog_event_ipc_t` (via type-generic selection), and the include guard;
at run time it verifies:

- the 26 kinds in declared order 0..25 and switch usability;
- constructor errors (NULL `out`, zero capacity, size-overflow →
  `COG_ERROR_NOMEM`, `*out` NULL on failure) and a working size-1 queue;
- `free(NULL)` no-op and leak-free create/push/pop/free cycles
  (ASan leak detection);
- NULL queries on `count`/`is_empty`/`is_full`;
- `push`/`pop` NULL argument validation with the queue unchanged;
- empty-pop returns `COG_ERROR_AGAIN` and leaves `*out` unchanged;
- value round-trip and copy semantics (source mutation invisible);
- NULL-payload events;
- payload reference semantics (buffer mutation visible after push);
- FIFO order with distinct payloads, fill-to-capacity, overflow,
  overflow-then-recovery, and interleaved push/pop;
- full push-3/pop-3 repeated cycles (1000x) and single-slot wrap cycles;
- IPC snapshot semantics for all three `COG_EVENT_IPC_*` kinds
  (struct copied, `msg_payload` borrowed) and IPC-kind NULL payload
  passthrough;
- const-correct inspectors on a live queue.
