# state.h Contract

`libraries/cog-core/include/cog/state.h` — state identifiers and the
table-driven dispatch contract, implemented by
`libraries/cog-core/src/state.c`.

This document records the executable contract verified by
`tests/unit/test_state.c`. It intentionally does **not** expand the
public API; it only documents what the header already declares.

## Status

Implemented. `src/state.c` provides the two declared functions; the
dispatch table itself is caller-provided `static const` data. Guard:
`COG_STATE_H`. Includes `<cog/event.h>` and `<cog/handler.h>`.

## Public types

| Type | Kind | Notes |
|------|------|-------|
| `cog_dispatch_entry_t` | `struct cog_dispatch_entry` | One row of the static dispatch table: `(state, event) → handler` plus an END-terminated allowed-transition list. |

Field order (pinned by `_Static_assert` in the test): `state`, `event`,
`handler`, `allowed_transitions`. All fields are resolved at compile or
link time; the table lives in read-only memory.

## Constants

| Constant | Value | Meaning (from header) |
|----------|-------|------------------------|
| `COG_TRANSITION_SELF` | `0xFFFE` | Reserved transition id: remain in the current state. |
| `COG_TRANSITION_END` | `0xFFFF` | Reserved sentinel that terminates an allowed-transition list. |
| `COG_STATE_MAX` | `0xFFFD` | Highest legal application state id. |

Invariants on the constants: `COG_STATE_MAX < COG_TRANSITION_SELF <
COG_TRANSITION_END`; `cog_state_t` and `cog_event_id_t` are 16-bit
unsigned ids (`uint16_t`). Application states must be in
`[0, COG_STATE_MAX]`. No event id values are reserved by the core.

## Macro

`COG_DISPATCH_ENTRY(state_, event_, handler_, transitions_)` expands to a
brace-enclosed initialiser that populates all four fields of a
`cog_dispatch_entry_t` in order.

## Functions

### `cog_state_lookup`

```c
cog_result_t cog_state_lookup(const cog_dispatch_entry_t *table, size_t table_size,
                              cog_state_t state, cog_event_id_t event,
                              const cog_dispatch_entry_t **out_entry);
```

Searches the dispatch table for the first row whose `(state, event)` pair
matches the query.

| Return | Condition |
|--------|-----------|
| `COG_OK` | A matching row was found; `*out_entry` (if non-NULL) points at it. |
| `COG_ERROR_NULL_PTR` | `table` is NULL and `table_size` is non-zero. |
| `COG_ERROR_NOT_FOUND` | No row matches, or the table is empty (`table_size == 0`, whether or not `table` is NULL). |

- `out_entry` may be NULL when only presence is required; it is written
  only on success and left unchanged on every failure.
- Precondition: `table` may be NULL only when `table_size` is zero.
- Lookup is a deterministic linear scan in table order: the **first**
  matching row wins. Duplicate `(state, event)` rows are legal; only the
  first is ever selected.
- Lookup is structural: it compares `state` and `event` only. A row whose
  `handler` is NULL still matches. A row that (incorrectly) uses a
  reserved state id is never matched by a well-formed query; reserved ids
  used as query values therefore always yield `COG_ERROR_NOT_FOUND`.
- The table must not be modified during the call.

### `cog_state_transition_allowed`

```c
cog_result_t cog_state_transition_allowed(const cog_state_t *allowed,
                                          cog_state_t transition);
```

Validates that `transition` appears in an END-terminated allowed list.

| Return | Condition |
|--------|-----------|
| `COG_OK` | `transition` is present in `allowed`. |
| `COG_ERROR_NULL_PTR` | `allowed` is NULL. |
| `COG_ERROR_TRANSITION_DENIED` | `transition` is not present in `allowed`. |

- The scan stops at the first `COG_TRANSITION_END` sentinel; entries
  after the sentinel are not part of the list.
- `COG_TRANSITION_END` itself is never a match: requesting it always
  returns `COG_ERROR_TRANSITION_DENIED`.
- `COG_TRANSITION_SELF` is an ordinary list member: it matches when it
  appears in the list and is denied otherwise.
- An empty list (`{COG_TRANSITION_END}`) denies every transition.
- Precondition: `allowed` must be END-terminated. A list that is not
  END-terminated is a caller error; it cannot be detected here without
  reading past the end of the array.
- The list must not be modified during the call.

## Error semantics

Both functions return `COG_OK` on success and a negative code on failure,
per `cog/result.h`. The only codes returned by this module are
`COG_OK`, `COG_ERROR_NULL_PTR`, `COG_ERROR_NOT_FOUND`, and
`COG_ERROR_TRANSITION_DENIED`. No other result codes are produced.

## Invariants

- The dispatch table is read-only after construction and may be shared by
  multiple core instances (per header).
- Lookup is deterministic: same table, same `(state, event)`, same result
  and same selected row, every call.
- `*out_entry` is written only on success and is never modified on
  failure.
- Reserved ids (`COG_TRANSITION_SELF`, `COG_TRANSITION_END`) are never
  valid application states and never valid transition requests (END).
- The module holds no global state and performs no allocation.

## Threading

Per header: the dispatch table is read-only after construction and may be
shared by multiple core instances. Individual core instances are not
thread-safe. Both functions require that their inputs (table rows,
allowed lists) are not modified during the call.

## Ownership

None. The module allocates nothing and owns nothing. Tables and allowed
lists are caller-provided static const data; `*out_entry` is a borrowed
pointer into the caller's table.

## Verification

`tests/unit/test_state.c` pins the constant values and struct field order
at compile time and verifies at run time: normal lookups for every row,
the `COG_DISPATCH_ENTRY` macro, NULL-handler rows, missing entries with
`*out_entry` unchanged, empty tables (NULL and non-NULL pointer), NULL
table with non-zero size, presence-only lookup with NULL `out_entry`,
boundary ids (`0`, `COG_STATE_MAX`, `0xFFFF`), reserved ids as query
values, deterministic first-match on duplicate rows, allowed/denied
transition validation including SELF and END sentinels, sentinel
termination of the list, empty allowed lists, boundary transition ids,
and every declared error path of both functions.

The test is compiled and run directly with strict warning flags and
ASan/UBSan; wiring into the CMake/CTest build is owned by integration
ticket 6044b51.
