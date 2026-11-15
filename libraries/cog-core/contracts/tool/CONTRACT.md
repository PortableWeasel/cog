# tool.h Contract

`libraries/cog-core/include/cog/tool.h` — tool descriptor table and tool
function contract.

This document records the executable contract verified by
`tests/unit/test_tool.c`. It intentionally does **not** expand the
public API; it only documents what the header already declares.

## Status

Implemented. `tool.h` declares one function, `cog_tool_table_lookup`,
implemented in `libraries/cog-core/src/tool.c`. Guard: `COG_TOOL_H`.
Includes `<cog/event.h>`, `<cog/result.h>`, and `<stddef.h>`.

## Public types

| Type | Kind | Notes |
|------|------|-------|
| `cog_tool_fn` | function pointer | Tool signature: `cog_result_t (*)(cog_core_t *, const char *, size_t, char *, size_t, size_t *)`. |
| `cog_tool_t` | `struct cog_tool` | Descriptor for a single tool. |
| `cog_tool_table_t` | `struct cog_tool_table` | Static tool table wrapper (pointer/count pair). |

## Struct layout

```c
typedef struct cog_tool {
    const char *name;         /**< Tool name used for dispatch. */
    const char *description;  /**< Human-readable description; may be NULL. */
    const char *input_schema; /**< Input validation schema; may be NULL. */
    cog_tool_fn handler;      /**< Tool implementation.  Non-NULL. */
} cog_tool_t;

typedef struct cog_tool_table {
    const cog_tool_t *tools; /**< Pointer to first tool descriptor. */
    size_t count;            /**< Number of descriptors.  May be zero. */
} cog_tool_table_t;
```

`cog_core_t` is forward-declared in `tool.h` to break the core/tool
include cycle; `cog_result_t` is defined in `result.h`.

## Functions

### `cog_tool_table_lookup`

```c
cog_result_t cog_tool_table_lookup(const cog_tool_table_t *table,
                                   const char *name,
                                   const cog_tool_t **out_tool);
```

Look up a tool by name in a tool table.

- `table` — tool table to search.
- `name` — tool name to look up; NUL-terminated.
- `out_tool` — receives a pointer to the matching descriptor on success;
  unchanged on failure. May be NULL if only presence is required.

Returns:

- `COG_OK` on success.
- `COG_ERROR_NULL_PTR` if `table` or `name` is NULL, or if the table is
  malformed (`tools == NULL` with `count > 0`).
- `COG_ERROR_NOT_FOUND` if no tool matches `name` (including an empty
  table).

## Lookup semantics

- Lookup is a deterministic linear scan in table order; the first
  descriptor whose `name` matches is selected.
- Names are compared as NUL-terminated strings. A row with a NULL `name`
  field is never matched by a well-formed query.
- On success, `*out_tool` points into the caller's table array; no
  ownership transfers and nothing is copied.
- On failure, `*out_tool` is left unchanged.
- An empty table (`count == 0`, with `tools` NULL or non-NULL) can never
  contain a match and returns `COG_ERROR_NOT_FOUND`.
- A malformed table (`tools == NULL` with `count > 0`) is a caller error
  and returns `COG_ERROR_NULL_PTR` (fail closed).

## Tool invocation conventions (from tool.h)

- A tool is a plain C function receiving the core instance and a request
  payload; it writes its result into a caller-provided buffer.
- The caller supplies the result buffer, so no hidden allocation occurs
  and ownership never transfers.
- If the buffer is too small, the tool reports the required length via
  `out_result_len` (when non-NULL) and returns `COG_ERROR_OVERFLOW`.
- `result` may be NULL only when `result_capacity` is zero.
- On success, `result` contains a NUL-terminated result string and
  ownership remains with the caller; on failure, `result` is unchanged.
- Tools must be re-entrant and must not introduce their own global
  mutable state.

## Preconditions

- `table` and `name` are non-NULL; `name` is NUL-terminated.
- The underlying tool array remains valid for the lifetime of any core
  that references it.
- The table must not be modified during the call.

## Threading

Per header: the tool table is read-only after construction and may be
shared by multiple core instances. Individual tool implementations must
be re-entrant and must not introduce their own global mutable state.
`cog_tool_table_lookup` performs no writes and is safe to call from any
thread provided the table is not concurrently modified.

## Ownership

The module holds no state, allocates nothing, and owns nothing. The
table and its descriptors are caller-provided and read-only; the
returned descriptor pointer aliases the caller's array and needs no
free function.

## Invariants

- `cog_tool_t.name` and `.handler` are documented as non-NULL for a
  well-formed row; `description` and `input_schema` may be NULL.
- `cog_tool_table_t.tools` is `const cog_tool_t *`; `.count` is `size_t`.
- Lookup is deterministic: for a fixed table and name, the result and
  the selected descriptor are stable.

## Verification

`tests/unit/test_tool.c` pins the struct field types at compile time and
at run time covers: lookup success (first matching descriptor returned
by pointer, including the second row and readable descriptor fields),
lookup not-found (with `out_tool` unchanged on failure), presence-only
lookup (`out_tool == NULL`), NULL table/name handling, empty tables
(`tools` NULL and non-NULL with `count == 0`), malformed table
(`tools == NULL`, `count > 0`), `cog_tool_fn` assignment and invocation,
overflow reporting through a test-defined tool, and NULL-core detection
by a test-defined tool. It does not test core.c tool dispatch or
toolkit.c; those modules own their own contracts.