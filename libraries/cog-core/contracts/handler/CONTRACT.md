# handler.h Contract

`libraries/cog-core/include/cog/handler.h` — handler result type and
handler function signature, plus the handler-dispatch primitives
implemented in `libraries/cog-core/src/handler.c`.

This document records the executable contract verified by
`tests/unit/test_handler.c`. It intentionally does **not** expand the
public API; it only documents what the headers already declare.

## Status

`handler.h` is a type-definition header: it declares the handler result
type and the handler function signature, but **no functions**. The
dispatch primitives that route a `(state, event)` pair to a handler are
declared in `state.h` and implemented in `handler.c` (ticket 9298184).
Guard: `COG_HANDLER_H`. Includes `<cog/event.h>`, `<cog/result.h>`, and
`<stdint.h>`.

## Public types

| Type | Kind | Notes |
|------|------|-------|
| `cog_handler_result_t` | `struct cog_handler_result` | Result returned by a state-machine handler. |
| `cog_handler_fn` | function pointer | Handler signature: `cog_handler_result_t (*)(cog_core_t *, const cog_event_t *)`. |

## Struct layout

```c
typedef struct cog_handler_result {
    cog_state_t transition; /**< Target state id or COG_TRANSITION_SELF. */
    cog_result_t status;    /**< Handler success/failure. */
} cog_handler_result_t;
```

## Constants (reserved transition ids, from state.h)

| Constant | Value | Meaning |
|----------|-------|---------|
| `COG_TRANSITION_SELF` | `0xFFFE` | Remain in the current state. |
| `COG_TRANSITION_END` | `0xFFFF` | Sentinel terminating an allowed-transition list. |
| `COG_STATE_MAX` | `0xFFFD` | Largest application state id. |

## Functions

`handler.h` declares no functions. The dispatch primitives implemented
in `handler.c` are declared in `state.h`:

| Function | Returns | Notes |
|----------|---------|-------|
| `cog_state_lookup(table, table_size, state, event, out_entry)` | `COG_OK` / `COG_ERROR_NULL_PTR` / `COG_ERROR_NOT_FOUND` | Find the dispatch row matching `(state, event)`. |
| `cog_state_transition_allowed(allowed, transition)` | `COG_OK` / `COG_ERROR_NULL_PTR` / `COG_ERROR_TRANSITION_DENIED` | Validate a requested transition against an allowed list. |

### `cog_state_lookup` error semantics

- `COG_OK` — a matching row was found; `*out_entry` (if non-NULL) points
  at it.
- `COG_ERROR_NULL_PTR` — `table` is NULL and `table_size` is non-zero.
- `COG_ERROR_NOT_FOUND` — no row matches, or the table is empty
  (`table` NULL and `table_size` zero, or `table_size` zero).
- `out_entry` may be NULL if only presence is required; on failure it is
  unchanged.

### `cog_state_transition_allowed` error semantics

- `COG_OK` — `transition` appears in the `COG_TRANSITION_END`-terminated
  `allowed` list.
- `COG_ERROR_NULL_PTR` — `allowed` is NULL.
- `COG_ERROR_TRANSITION_DENIED` — `transition` is not in the list.
- `COG_TRANSITION_END` is a sentinel, never a valid transition, so it is
  always denied.

## Handler semantics (from handler.h)

- A handler returns a `cog_handler_result_t` describing the requested
  transition and its status.
- A non-`COG_OK` status prevents the transition from being applied and
  is reported by `cog_core_run`.
- `transition` is either `COG_TRANSITION_SELF` or an id that appears in
  the current dispatch entry's allowed list.
- Handlers are wired into static const dispatch tables (state.h); there
  is no dynamic registration.
- Each handler must be re-entrant and must not retain global mutable
  state between calls.
- Handlers are invoked on the thread running `cog_core_run` and must not
  block indefinitely.
- Preconditions: `core` is a fully initialised core instance; `event` is
  non-NULL and valid for the duration of the call. A handler that
  receives a NULL `core` or NULL `event` may report `COG_ERROR_NULL_PTR`.

## Invariants

- `COG_TRANSITION_SELF == 0xFFFE` and `COG_TRANSITION_END == 0xFFFF`;
  application states are in `[0, COG_STATE_MAX]`.
- A zero-initialized `cog_handler_result_t` is a valid default:
  `status == COG_OK` (success) and `transition == 0` (state 0, not
  `COG_TRANSITION_SELF`, which is `0xFFFE`).
- The dispatch table and allowed-transition lists are read-only after
  construction and may be shared by multiple core instances.
- `cog_state_lookup` does not modify the table and leaves `*out_entry`
  unchanged on failure.

## Threading

Per headers: the dispatch table is read-only after construction and may
be shared by multiple core instances. Individual core instances are not
thread-safe. The dispatch functions must not be called concurrently with
a mutation of the table (there is none after construction).

## Ownership

The dispatch functions allocate nothing and hold no state. The table and
allowed-transition lists are caller-owned and must outlive any core that
references them.

## Module ownership note

`cog_state_lookup` and `cog_state_transition_allowed` are declared in
`state.h` but implemented in `handler.c` because ticket 9298184 owns
handler dispatch. The parallel `state.c` ticket (6b1c97b) also targets
these functions; integration ticket 6044b51 resolves the overlap by
keeping a single implementation.

## Verification

`tests/unit/test_handler.c` pins the struct field types and reserved
transition values at compile time, and at run time covers: successful
dispatch, `out_entry == NULL`, not-found, NULL table (non-zero and zero
size), empty table, allowed/denied/self/end-sentinel transitions, NULL
allowed list, handler success, handler self-transition, handler error
propagation, NULL core/event handling, `cog_handler_fn` assignment and
invocation, and the zero-initialized default result.