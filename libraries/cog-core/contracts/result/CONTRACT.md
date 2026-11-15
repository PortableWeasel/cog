# result.h Contract

`libraries/cog-core/include/cog/result.h` — error codes and result type
shared across cog-core.

This document records the executable contract verified by
`tests/unit/test_result.c`. It intentionally does **not** expand the
public API; it only documents what the header already declares.

## Status

Header-only. No functions, no state, no implementation units required.
Guard: `COG_RESULT_H`.

## Public types

| Type | Kind | Notes |
|------|------|-------|
| `cog_result_t` | `enum cog_result` | Returned by every public cog-core function |

## Constants (error codes)

All values are stable: they are part of the ABI of the module and must
never be renumbered. New codes are appended at the end of the
enumeration.

| Constant | Value | Meaning (from header) |
|----------|-------|------------------------|
| `COG_OK` | `0` | Success. |
| `COG_ERROR_INVALID_ARG` | `-1` | A required argument was NULL, out of range, or malformed. |
| `COG_ERROR_NULL_PTR` | `-2` | A required pointer argument was NULL. |
| `COG_ERROR_NOMEM` | `-3` | Memory allocation failed. |
| `COG_ERROR_NOT_FOUND` | `-4` | The requested item does not exist. |
| `COG_ERROR_OVERFLOW` | `-5` | A buffer or counter would overflow. |
| `COG_ERROR_UNHANDLED` | `-6` | No handler is registered for the current (state, event) pair. |
| `COG_ERROR_TRANSITION_DENIED` | `-7` | The requested state transition is not listed as allowed for the current entry. |
| `COG_ERROR_AGAIN` | `-8` | The operation could not complete now; the caller may retry later. |
| `COG_ERROR_INVALID_STATE` | `-9` | The object is in the wrong state for this call. |
| `COG_ERROR_IO` | `-10` | An input/output operation failed. |
| `COG_ERROR_ABORTED` | `-11` | The operation was cancelled or the run loop was stopped. |
| `COG_ERROR_TIMEOUT` | `-12` | The operation exceeded its time limit. |
| `COG_ERROR_UNAVAILABLE` | `-13` | The requested resource is temporarily unavailable (e.g. rate limited). |

## Functions

None. The module defines the type and constants only.

## Error semantics

- `COG_OK == 0`; every failure code is negative and distinct.
- A caller tests success with `r == COG_OK` (or `r != COG_OK` for
  failure); there is no "positive" partial-success encoding.
- `COG_ERROR_AGAIN` signals a retry-eligible operation; `COG_ERROR_UNAVAILABLE`
  signals a temporarily unavailable resource such as a rate limit.
  Exact retry policy is defined by the calling module, not by result.h.
- All other codes are terminal for the operation that returned them
  unless the calling module's contract says otherwise.

## Invariants

- Zero means success; negative values mean failure.
- Codes are stable and extensible: new codes are appended at the end of
  the enumeration without renumbering existing values.
- Every public function in cog-core returns `cog_result_t` (declared in
  the header's `@brief`; enforced at each API surface individually).

## Threading

None. The constants are pure values; no threading constraints apply
(per header).

## Ownership

None. The module holds no state, allocates nothing, and owns nothing.

## Verification

`tests/unit/test_result.c` pins every documented value with
`_Static_assert` at compile time and checks at run time that all codes
are negative (except `COG_OK == 0`), pairwise distinct, and usable in
int-promoted switch/equality contexts.
