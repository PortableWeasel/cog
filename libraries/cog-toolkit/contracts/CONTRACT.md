# cog-toolkit Contract

`libraries/cog-toolkit/include/cog/toolkit.h` — tool execution framework
(call context, execution dispatcher, result collection).

This document records the executable contract verified by
`tests/unit/test_toolkit.c`. It intentionally does **not** expand the
public API; it only documents what the headers already declare and how the
implementation (`libraries/cog-toolkit/src/`) satisfies it.

## Status

Implemented. `cog-toolkit` is a static library compiling
`src/toolkit_call.c`, `src/toolkit_exec.c`, and `src/toolkit_result.c`.
Guards: `COG_TOOLKIT_H`, `COG_TOOLKIT_CALL_H`, `COG_TOOLKIT_EXEC_H`,
`COG_TOOLKIT_RESULT_H`. The umbrella header `cog/toolkit.h` includes the
three sub-headers.

## Public types

| Type | Kind | Notes |
|------|------|-------|
| `cog_toolkit_call_t` | opaque struct | Created by `cog_toolkit_call_new`, destroyed by `cog_toolkit_call_free`. |
| `cog_toolkit_exec_opts_t` | struct | Execution options (timeout, max_parallel, stop_on_error). |
| `cog_toolkit_result_t` | struct | Value type describing one tool outcome. |
| `cog_toolkit_result_collection_t` | opaque struct | Created by `cog_toolkit_execute_all`, destroyed by `cog_toolkit_result_collection_free`. |

`cog_core_t` is forward-declared in the toolkit headers to break the
core/toolkit include cycle.

## Call context (`toolkit_call.h`)

### `cog_toolkit_call_new`

```c
cog_result_t cog_toolkit_call_new(cog_toolkit_call_t **out,
                                  const char *name,
                                  const char *request,
                                  size_t request_len);
```

- All storage is allocated in a single heap block (the struct plus a
  NUL-terminated copy of `name`). The `request` payload is **borrowed**,
  never copied; the caller must keep it alive until execution completes.
- Returns `COG_OK`, `COG_ERROR_NULL_PTR` (`out` or `name` NULL),
  `COG_ERROR_INVALID_ARG` (`request` NULL with non-zero `request_len`),
  `COG_ERROR_NOMEM`.
- On failure `*out` is NULL and unchanged.

### `cog_toolkit_call_free`

- Frees the single heap block. The borrowed `request` is not freed.
- NULL is a no-op.

### `cog_toolkit_call_set_timeout`

- Records a per-call wall-clock timeout in ms. Zero means "use the
  dispatcher default".
- Returns `COG_ERROR_INVALID_ARG` for values above
  `0x7FFFFFFF` ms (~24.8 days), a documented implementation limit chosen
  to stay below any 32-bit tick overflow.

### `cog_toolkit_call_cancel` / `cog_toolkit_call_is_cancelled`

- Cooperative cancellation: `cancel` sets the flag; `is_cancelled` reads
  it. The dispatcher observes the flag at its cancellation points.

### `cog_toolkit_call_name` / `cog_toolkit_call_request`

- `name` returns the NUL-terminated name (valid until free).
- `request` returns the borrowed payload, or NULL when `call` is NULL or
  `request_len` is zero. `out_len` (optional) receives the length.

## Execution dispatcher (`toolkit_exec.h`)

### `cog_toolkit_exec_opts_t`

| Field | Meaning | Default when zero |
|-------|---------|-------------------|
| `timeout_ms` | Per-call wall-clock timeout. | 60000 (dispatcher default) |
| `max_parallel` | Max concurrent calls. | `call_count` |
| `stop_on_error` | Stop launching new calls on first failure. | false |

For `max_parallel == 1` (or at most one call), calls run sequentially on the
caller thread. Otherwise the dispatcher starts up to
`min(max_parallel, call_count)` POSIX workers and joins every launched worker
before returning. A mutex-protected index assigns each call exactly once.
Workers write only their claimed call's result slots; collection assembly and
string copies occur on the caller thread after all joins. Tools are
synchronous C functions that cannot be preempted. With concurrent execution,
tool handlers must be reentrant and must synchronise any mutation of the
shared core or other shared state.

### `cog_toolkit_execute`

```c
cog_result_t cog_toolkit_execute(cog_core_t *core,
                                 const cog_tool_table_t *table,
                                 cog_toolkit_call_t *call,
                                 cog_toolkit_result_t *result,
                                 size_t result_capacity);
```

- Resolves `call->name` in `table` (static lookup, no dynamic
  registration), invokes the tool with the request payload, and writes the
  outcome into `result->output` (a caller-provided buffer of at least
  `result_capacity` bytes).
- Returns `COG_OK`, `COG_ERROR_NULL_PTR` (any required pointer NULL,
  including `result->output`), `COG_ERROR_INVALID_ARG` (`result_capacity`
  zero), `COG_ERROR_NOT_FOUND` (unknown tool), `COG_ERROR_ABORTED`
  (cancelled before or during execution), or an error propagated from the
  tool.
- On success `result->status` is `COG_OK`, `result->output` holds a
  NUL-terminated string, and `result->output_len` is its length. On
  failure `result->status` is non-zero and the output buffer is unchanged.
- The caller-provided buffer is used directly; no growth is performed
  (a too-small buffer surfaces the tool's `COG_ERROR_OVERFLOW`).
- Single-call execution does not enforce a wall-clock timeout (not a
  documented return code); timeout handling is the responsibility of
  `cog_toolkit_execute_all`.

### `cog_toolkit_execute_all`

```c
cog_result_t cog_toolkit_execute_all(cog_core_t *core,
                                     const cog_tool_table_t *table,
                                     cog_toolkit_call_t *const *calls,
                                     size_t call_count,
                                     const cog_toolkit_exec_opts_t *opts,
                                     cog_toolkit_result_collection_t **out_results);
```

- Dispatches each call and collects results into an opaque collection that
  owns copies of the output strings and error messages.
- Returns `COG_OK`, `COG_ERROR_NULL_PTR` (`core`, `table`, `calls`, or
  `out_results` NULL), `COG_ERROR_INVALID_ARG` (a NULL entry in `calls`
  when `call_count` is non-zero), `COG_ERROR_NOMEM`, or the first failing
  result code when `opts->stop_on_error` is true.
- On success `*out_results` contains **exactly `call_count` results in the
  same order as `calls`**. `call_count` may be zero (empty collection).
- On failure `*out_results` is NULL and no resources are held.

#### Timeout handling

- Each call's effective timeout is `call->timeout_ms` if non-zero, else
  `opts->timeout_ms` if non-zero, else the dispatcher default (60000 ms).
- A per-call wall-clock deadline is computed from a monotonic clock and
  checked after the tool returns. A tool that overruns its budget is
  recorded as `COG_ERROR_TIMEOUT` with an error message. Because tools
  cannot be preempted, the timeout is advisory and enforced at invocation
  boundaries.

#### Cancellation

- A call cancelled before invocation is recorded as `COG_ERROR_ABORTED`
  without invoking the tool. A call cancelled during execution is recorded
  as `COG_ERROR_ABORTED` after the tool returns (cooperative).

#### `stop_on_error`

- After the first observed non-OK result (tool error, not-found, timeout,
  or abort), no further calls are claimed. Already-running tools finish and
  every launched worker is joined. The final return is the earliest failing
  result **by input index**, not completion order, and `*out_results` is
  NULL. With concurrency, calls claimed before the failure was observed may
  run even when their input index follows the failing call.

## Result types (`toolkit_result.h`)

### `cog_toolkit_result_t`

| Field | Meaning |
|-------|---------|
| `status` | `COG_OK` on success; any other value indicates failure. |
| `output` | NUL-terminated result string, or NULL on failure / empty output. |
| `output_len` | Length of `output` excluding the NUL. |
| `error_message` | Optional human-readable error, or NULL. |

For single invocations the caller owns the output buffer. For collections
the collection owns the buffers; `output` and `error_message` point into
the collection's storage and remain valid until the collection is freed.

### `cog_toolkit_result_collection_count` / `_get` / `_free`

- `count` returns the number of results (0 for NULL).
- `get` returns `COG_OK` and `*out_result` (may be NULL to validate only),
  `COG_ERROR_NULL_PTR` for a NULL collection, `COG_ERROR_INVALID_ARG` for
  an out-of-range index.
- `free` releases the results array and the storage block; NULL is a no-op.

### `cog_toolkit_result_is_ok`

- True iff `result` is non-NULL and `result->status == COG_OK`.

### `cog_toolkit_result_validate_schema`

- The schema must contain exactly one JSON object. Trailing whitespace is
  accepted; any other trailing characters are malformed.
- Implements a small, documented JSON Schema subset:
  - `type`: `object`, `array`, `string`, `number`, `integer` (treated as
    number), `boolean`, `null`.
  - `required`: array of property names, enforced when the output is an
    object.
  - Unknown keywords are ignored.
- A NULL or empty schema means "no validation required" and returns
  `COG_OK`.
- Returns `COG_ERROR_NULL_PTR` (`result` NULL), `COG_ERROR_INVALID_STATE`
  (`result->status` not `COG_OK`), `COG_ERROR_INVALID_ARG` (`output` NULL,
  malformed schema, output not valid JSON, type mismatch, or a missing
  required property).
- Schemas listing more than 64 required properties are rejected as
  malformed/unsupported.

## Error semantics

- Every public function returns `cog_result_t` (or void/bool where
  documented). Errors are propagated, never swallowed.
- The library never allocates behind the caller's back: call contexts and
  result collections are created by explicit constructors and destroyed by
  matching destructors. The request payload is borrowed.
- No global mutable state; no dynamic registration. Tools are resolved
  exclusively through the static const `cog_tool_table_t`.