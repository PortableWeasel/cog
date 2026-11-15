# handler.h Contract

`libraries/cog-core/include/cog/handler.h` — handler result type and
handler function signature.

This document records the executable contract verified by
`tests/unit/test_handler.c`. It intentionally does **not** expand the
public API; it only documents what the header already declares.

## Status

Header-only. `handler.h` declares **no functions** and owns **no
implementation unit**; there is no `handler.c`. Guard: `COG_HANDLER_H`.
Includes `<cog/event.h>`, `<cog/result.h>`, and `<stdint.h>`.

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

`cog_state_t` is defined in `event.h`; `cog_result_t` in `result.h`.
`COG_TRANSITION_SELF` is defined in `state.h` and referenced by the
`transition` field documentation.

## Functions

None. The module defines the types only.

## Handler invocation conventions (from handler.h)

- A handler is a pure function receiving the core instance and the event
  that triggered it; it performs the work associated with the current
  state and returns the state it would like to transition to.
- The returned `transition` is either `COG_TRANSITION_SELF` or an id
  that appears in the current dispatch entry's allowed list.
- The returned `status` tells the dispatcher whether the handler
  succeeded. A non-`COG_OK` status prevents the transition from being
  applied and is reported by `cog_core_run`.
- Handlers are wired into static const dispatch tables (see `state.h`);
  there is no dynamic registration.
- Each handler must be re-entrant and must not retain global mutable
  state between calls.
- Handlers are invoked on the thread running `cog_core_run` and must not
  block indefinitely; long-running work should be delegated to an
  auxiliary process or returned as an asynchronous event.

## Preconditions

- `core` is a fully initialised core instance.
- `event` is non-NULL and valid for the duration of the call.

A handler that receives a NULL `core` or NULL `event` (a precondition
violation) may report `COG_ERROR_NULL_PTR` in its `status`.

## Result semantics

- `status == COG_OK` means success; any non-`COG_OK` value is a failure
  that prevents the requested transition from being applied.
- A zero-initialized `cog_handler_result_t` is a valid default:
  `status == COG_OK` (success) and `transition == 0` (state 0, not
  `COG_TRANSITION_SELF`, which is `0xFFFE`).

## Threading

Per header: handlers are invoked on the same thread that runs
`cog_core_run`. They must not block indefinitely. The dispatch table is
read-only after construction and may be shared by multiple core
instances; individual core instances are not thread-safe.

## Ownership

The module holds no state, allocates nothing, and owns nothing. The
`cog_handler_result_t` is a caller-allocated value returned by value; it
needs no constructor, destructor, or free function.

## Invariants

- `cog_handler_result_t.transition` is a `cog_state_t`; `.status` is a
  `cog_result_t`.
- `COG_TRANSITION_SELF == 0xFFFE` (referenced by the `transition` doc).
- A zero-initialized result is valid: `status == COG_OK`,
  `transition == 0`.

## Verification

`tests/unit/test_handler.c` pins the struct field types and
`COG_TRANSITION_SELF` at compile time, and at run time covers: the
zero-initialized default result, `cog_handler_fn` assignment and
invocation through a test-defined handler, context/event propagation
(the handler receives and reads the event), next-state passthrough,
self-transition, status passthrough (non-`COG_OK`), and NULL core/event
handling. It does not test `state.h` dispatch functions; `state.c` owns
those.