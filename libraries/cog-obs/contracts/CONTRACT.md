# cog-obs Contract

`libraries/cog-obs/include/cog/obs.h` and
`libraries/cog-obs/include/cog/obs_types.h` — structured logging, trace
spans, connection diagnostics, and the connection event stream.

This document records the executable contract verified by
`tests/unit/test_obs.c` and `tests/unit/test_obs_types.c` and implemented
by `libraries/cog-obs/src/obs.c` and `libraries/cog-obs/src/obs_types.c`.
It intentionally does **not** expand the public API; it only documents
what the headers already declare.

## Status

**Implemented** (`libraries/cog-obs/src/obs.c`,
`libraries/cog-obs/src/obs_types.c`). One caller-allocated context
(`cog_obs_t`) owns a record scratch buffer, a span stack, and a
fixed-capacity diagnostic table; all three are allocated in a **single
heap block** by `cog_obs_new` and released by `cog_obs_free`. There is no
global instance, no constructor attribute, and no allocation on any log,
span, diagnostic, or event call.
Guard: `COG_OBS_H` / `COG_OBS_TYPES_H`. Includes `<cog/result.h>`,
`<stdbool.h>`, `<stddef.h>`, `<stdint.h>`.

## Public types

| Type | Kind | Notes |
|------|------|-------|
| `cog_obs_t` | `struct cog_obs` (opaque) | Observability context; definition lives in `src/obs.c`. |
| `cog_obs_severity_t` | `enum cog_obs_severity` | Log severity, most verbose first. |
| `cog_obs_field_kind_t` | `enum cog_obs_field_kind` | Value kind of a structured field. |
| `cog_obs_field_desc_t` | `struct cog_obs_field_desc` | Static descriptor: name + declared kind. |
| `cog_obs_field_table_t` | `struct cog_obs_field_table` | Static const array of descriptors. |
| `cog_obs_field_t` | `struct cog_obs_field` | One value supplied per log call (index + kind + union). |
| `cog_obs_span_id_t` | `typedef uint64_t` | Trace span id; zero is reserved (`COG_OBS_SPAN_ID_NONE`). |
| `cog_obs_span_record_t` | `struct cog_obs_span_record` | Completed span handed to the trace sink. |
| `cog_obs_sink_t` | `struct cog_obs_sink` | One output destination (name, callback, user, threshold). |
| `cog_obs_sink_table_t` | `struct cog_obs_sink_table` | Static const array of sinks. |
| `cog_obs_config_t` | `struct cog_obs_config` | Construction parameters; zero-init gives defaults. |
| `cog_obs_conn_state_t` | `enum cog_obs_conn_state` | Connection state (stable numeric values). |
| `cog_obs_conn_snapshot_t` | `struct cog_obs_conn_snapshot` | Caller-owned point-in-time connection snapshot. |
| `cog_obs_conn_event_kind_t` | `enum cog_obs_conn_event_kind` | Connection event kind (GUI stream). |
| `cog_obs_conn_event_t` | `struct cog_obs_conn_event` | One connection event (deltas + totals). |

## Constants

Declared order and values (pinned by `tests/unit/test_obs_types.c` and
`tests/unit/test_obs.c`):

| Constant | Value |
|----------|-------|
| `COG_OBS_TRACE` | `0` |
| `COG_OBS_DEBUG` | `1` |
| `COG_OBS_INFO` | `2` |
| `COG_OBS_WARN` | `3` |
| `COG_OBS_ERROR` | `4` |
| `COG_OBS_FATAL` | `5` |
| `COG_OBS_SEVERITY_COUNT` | `6` |
| `COG_OBS_FIELD_STRING` | `0` |
| `COG_OBS_FIELD_INT` | `1` |
| `COG_OBS_FIELD_UINT` | `2` |
| `COG_OBS_FIELD_BOOL` | `3` |
| `COG_OBS_FIELD_DOUBLE` | `4` |
| `COG_OBS_SPAN_ID_NONE` | `0` |
| `COG_OBS_CONN_IDLE` | `0` |
| `COG_OBS_CONN_CONNECTING` | `1` |
| `COG_OBS_CONN_ACTIVE` | `2` |
| `COG_OBS_CONN_STALLED` | `3` |
| `COG_OBS_CONN_CLOSED` | `4` |
| `COG_OBS_CONN_FAILED` | `5` |
| `COG_OBS_CONN_EVENT_OPEN` | `0` |
| `COG_OBS_CONN_EVENT_CONNECTED` | `1` |
| `COG_OBS_CONN_EVENT_PROGRESS` | `2` |
| `COG_OBS_CONN_EVENT_STALLED` | `3` |
| `COG_OBS_CONN_EVENT_RETRY` | `4` |
| `COG_OBS_CONN_EVENT_CLOSED` | `5` |
| `COG_OBS_CONN_EVENT_FAILED` | `6` |

## Severity filter semantics

The numeric order is significant and is the same for the context and for
each sink:

- **Context filter** (`config.min_severity`): a record is recorded iff
  `severity >= min_severity`. `COG_OBS_TRACE` (0) therefore records
  everything; `COG_OBS_WARN` (3) records WARN, ERROR, FATAL only.
  Records more verbose than the threshold are discarded **before**
  formatting.
- **Sink filter** (`sink.min_severity`): a record is delivered to a sink
  iff `severity >= sink.min_severity`. A sink with a stricter threshold
  may discard a record the context admitted.
- `cog_obs_enabled` reports the context filter only.

## Record formatting

`cog_obs_log` / `cog_obs_log_to` format exactly one line:

```
<timestamp_ns> <severity> [<service>] <message> [<key>=<value> ...]
```

- `timestamp_ns` is the context clock reading in decimal nanoseconds.
- `severity` is the lowercase name (`trace`, `debug`, `info`, `warn`,
  `error`, `fatal`).
- `service` is emitted only when `config.service` is non-NULL.
- Fields appear in caller order as `key=value` with a single space
  separator; the key is the descriptor's `name`.
- Value rendering: STRING is quoted and escaped; INT is signed decimal;
  UINT is unsigned decimal; BOOL is `true`/`false`; DOUBLE is `%.6f`
  (six fractional digits).
- String escaping: `"` → `\"`, `\` → `\\`, `\n` → `\n`, `\r` → `\r`,
  `\t` → `\t` (backslash-letter sequences in the output). No other
  characters are transformed.

Formatting happens once into the context-owned scratch buffer
(1024 bytes, a private constant); the same text is offered to every
eligible sink and, for `cog_obs_log_to`, copied to the caller's buffer.

## Diagnostic line formatting

`cog_obs_diag_format` renders one snapshot as a single line without a
trailing newline:

```
conn=<id> state=<name> [endpoint=<e>] [provider=<p>] in=<n> out=<n> retries=<n> age_ms=<n> idle_ms=<n> status=<code>
```

- `state` is the lowercase state name (`idle`, `connecting`, `active`,
  `stalled`, `closed`, `failed`).
- `endpoint` and `provider` are omitted when their pointer is NULL.
- `age_ms` = `(now - started_ns) / 1_000_000`; `idle_ms` =
  `(now - last_progress_ns) / 1_000_000`, both against the context
  clock.
- `status` is the numeric `cog_result_t` code (0 for `COG_OK`, negative
  for errors).

## Functions

| Function | Signature | Errors |
|----------|-----------|--------|
| `cog_obs_severity_name` | `cog_result_t (cog_obs_severity_t, const char **out_name)` | `COG_ERROR_NULL_PTR` (`out_name` NULL); `COG_ERROR_INVALID_ARG` (severity out of range) |
| `cog_obs_clock_now_ns` | `cog_result_t (uint64_t *out_now_ns)` | `COG_ERROR_NULL_PTR` (`out_now_ns` NULL); `COG_ERROR_IO` (platform clock failure) |
| `cog_obs_new` | `cog_result_t (cog_obs_t **out, const cog_obs_config_t *config)` | `COG_ERROR_NULL_PTR` (`out` or `config` NULL); `COG_ERROR_INVALID_ARG` (severity out of range; fields/sinks table with NULL array and non-zero count; any configured sink with a NULL `deliver` callback); `COG_ERROR_NOMEM` (allocation fails or the single-block size computation would wrap) |
| `cog_obs_free` | `void (cog_obs_t *obs)` | none; `obs == NULL` is a no-op |
| `cog_obs_log` | `cog_result_t (cog_obs_t *, cog_obs_severity_t, const char *message, const cog_obs_field_t *fields, size_t field_count)` | `COG_ERROR_NULL_PTR` (`obs` or `message` NULL); `COG_ERROR_INVALID_ARG` (severity out of range, fields NULL with non-zero count, field index out of range, kind mismatch, NULL string value); `COG_ERROR_OVERFLOW` (record exceeds the scratch buffer); `COG_ERROR_IO` (a sink rejected the record) |
| `cog_obs_log_to` | `cog_result_t (cog_obs_t *, cog_obs_severity_t, const char *, const cog_obs_field_t *, size_t, char *out_buf, size_t buf_capacity, size_t *out_len)` | as `cog_obs_log`, plus `COG_ERROR_NULL_PTR` (`out_buf` NULL with non-zero `buf_capacity`) and `COG_ERROR_OVERFLOW` when the record does not fit `buf_capacity` (with `*out_len` = required capacity) |
| `cog_obs_enabled` | `cog_result_t (const cog_obs_t *, cog_obs_severity_t, int *out_enabled)` | `COG_ERROR_NULL_PTR` (`obs` or `out_enabled` NULL); `COG_ERROR_INVALID_ARG` (severity out of range) |
| `cog_obs_trace_begin` | `cog_result_t (cog_obs_t *, const char *name, cog_obs_span_id_t *out_id)` | `COG_ERROR_NULL_PTR` (`obs`, `name`, or `out_id` NULL); `COG_ERROR_OVERFLOW` (span stack at capacity); `COG_ERROR_IO` (clock failure) |
| `cog_obs_trace_end` | `cog_result_t (cog_obs_t *, cog_obs_span_id_t id, cog_result_t status)` | `COG_ERROR_NULL_PTR` (`obs` NULL); `COG_ERROR_NOT_FOUND` (no open span with `id`); `COG_ERROR_INVALID_ARG` (`id` is open but not innermost); `COG_ERROR_IO` (clock failure or trace sink rejection) |
| `cog_obs_trace_current` | `cog_result_t (const cog_obs_t *, cog_obs_span_id_t *out_id)` | `COG_ERROR_NULL_PTR` (`obs` or `out_id` NULL) |
| `cog_obs_trace_depth` | `cog_result_t (const cog_obs_t *, size_t *out_depth)` | `COG_ERROR_NULL_PTR` (`obs` or `out_depth` NULL) |
| `cog_obs_diag_publish` | `cog_result_t (cog_obs_t *, const cog_obs_conn_snapshot_t *snapshot)` | `COG_ERROR_NULL_PTR` (`obs` or `snapshot` NULL); `COG_ERROR_INVALID_ARG` (`conn_id == 0` or state out of range); `COG_ERROR_OVERFLOW` (table full, no matching id) |
| `cog_obs_diag_clear` | `cog_result_t (cog_obs_t *, uint64_t conn_id)` | `COG_ERROR_NULL_PTR` (`obs` NULL); `COG_ERROR_INVALID_ARG` (`conn_id == 0`); `COG_ERROR_NOT_FOUND` (no slot holds `conn_id`) |
| `cog_obs_diag_at` | `cog_result_t (const cog_obs_t *, size_t index, cog_obs_conn_snapshot_t *out_snapshot)` | `COG_ERROR_NULL_PTR` (`obs` or `out_snapshot` NULL); `COG_ERROR_INVALID_ARG` (`index >=` occupied count) |
| `cog_obs_diag_count` | `cog_result_t (const cog_obs_t *, size_t *out_count)` | `COG_ERROR_NULL_PTR` (`obs` or `out_count` NULL) |
| `cog_obs_diag_count_state` | `cog_result_t (const cog_obs_t *, cog_obs_conn_state_t state, size_t *out_count)` | `COG_ERROR_NULL_PTR` (`obs` or `out_count` NULL); `COG_ERROR_INVALID_ARG` (state out of range) |
| `cog_obs_diag_format` | `cog_result_t (const cog_obs_t *, const cog_obs_conn_snapshot_t *, char *out_buf, size_t buf_capacity, size_t *out_len)` | `COG_ERROR_NULL_PTR` (`obs`, `snapshot`, or `out_buf` NULL); `COG_ERROR_INVALID_ARG` (state out of range); `COG_ERROR_OVERFLOW` (line does not fit `buf_capacity`, with `*out_len` = required capacity); `COG_ERROR_IO` (clock failure) |
| `cog_obs_conn_event_emit` | `cog_result_t (cog_obs_t *, const cog_obs_conn_event_t *event)` | `COG_ERROR_NULL_PTR` (`obs` or `event` NULL); `COG_ERROR_INVALID_ARG` (kind out of range); `COG_ERROR_IO` (sink rejection) |

## Ownership and storage semantics

- **One heap block per context.** `cog_obs_new` performs exactly one
  `malloc` of `sizeof(struct cog_obs) + diag_capacity * sizeof(snapshot)
  + span_capacity * sizeof(span_entry)`; the diagnostic table and span
  stack live inside that block. `cog_obs_free` performs exactly one
  `free`. On every `new` failure `*out` is NULL and no resources are
  held.
- **Defaults.** `diag_capacity == 0` selects 16 slots; `span_capacity ==
  0` selects 32. `clock_ns == NULL` selects `cog_obs_clock_now_ns`.
- **Borrowed strings.** Field descriptors, sink names, service name,
  span names, and snapshot/event strings are borrowed, never copied.
  The caller guarantees their lifetime; snapshot strings must remain
  valid until the slot is republished or cleared.
- **No allocation on the hot path.** Logging formats into the
  context-owned scratch buffer; span and diagnostic calls touch only
  preallocated arrays; event emission only calls the sink.
- **Static sinks.** Sinks come from a static const table at
  construction; there is no runtime registration. Fail closed: a
  configured sink whose `deliver` callback is NULL is rejected by
  `cog_obs_new` with `COG_ERROR_INVALID_ARG` (a sink without a callback
  could not deliver anything, and silently dropping every record would
  hide a misconfiguration). The rejection matches the header's
  `cog_obs_sink_t::deliver` and `cog_obs_new` contracts.
- **Span ids** start at 1 and increment; `COG_OBS_SPAN_ID_NONE` (0) is
  never handed out. Id exhaustion (wrap to 0) makes `trace_begin`
  return `COG_ERROR_OVERFLOW`.
- **Diagnostic slots** are free when `conn_id == 0`; `publish` matches
  on `conn_id` (update in place) and otherwise takes the first free
  slot in slot order. `diag_at` enumerates occupied slots in slot
  order, skipping gaps.

## Error semantics

- `new`: NULL `out` or `config` → `COG_ERROR_NULL_PTR`; severity out of
  range, a fields/sinks table with a NULL array and non-zero count, or
  any configured sink with a NULL `deliver` callback →
  `COG_ERROR_INVALID_ARG`; allocation failure or size-wrap →
  `COG_ERROR_NOMEM`. `*out` is set to NULL before any validation, so it
  is NULL on every failure.
- `log`/`log_to`: validation failures leave nothing formatted and
  nothing delivered. A record filtered by the context threshold returns
  `COG_OK` without formatting (and, for `log_to`, writes an empty
  string and `*out_len == 0`). Scratch overflow returns
  `COG_ERROR_OVERFLOW` with no delivery. Sink rejection returns
  `COG_ERROR_IO` **after** every eligible sink has been offered the
  record.
- `log_to` overflow: `*out_len` receives the required capacity
  (excluding the NUL terminator) so the caller can retry with a buffer
  of `required + 1` bytes. `out_buf == NULL` with `buf_capacity == 0`
  is legal (log-only); `out_buf == NULL` with non-zero capacity is
  `COG_ERROR_NULL_PTR`.
- `trace_begin`: on failure the span stack is unchanged.
- `trace_end`: `COG_ERROR_NOT_FOUND` when no open span has `id`;
  `COG_ERROR_INVALID_ARG` when `id` is open but not the innermost span
  (spans must close in reverse order). On either failure the stack is
  unchanged. If the trace sink rejects the record, the span is
  **restored** to the stack and `COG_ERROR_IO` is returned, so the
  caller may retry the end.
- `diag_publish`: on failure the table is unchanged. A full table is
  `COG_ERROR_OVERFLOW`, never a silent drop; updating an existing id on
  a full table still succeeds.
- `diag_clear`: on success the slot is zeroed and free for reuse.
- `conn_event_emit`: with no sink configured the call is a no-op
  returning `COG_OK`.

## Invariants

- `diag_count` is always in `[0, diag_capacity]`; `span_depth` is always
  in `[0, span_capacity]`.
- The span stack is strictly nested: `trace_end` accepts only the
  innermost open span.
- A zero-initialized `cog_obs_config_t` is a valid configuration
  (defaults apply).
- The context is not thread-safe; sinks, field tables, and message ids
  are read-only after construction and may be shared between contexts
  and threads.
- `cog_obs_severity_names` and `cog_obs_conn_state_names` are private
  `static const` tables in the implementation (indexer-friendly, no
  dynamic registration), matching the project's dispatch-table
  convention.

## Threading

Per header: a `cog_obs_t` is **not thread-safe** — it owns a mutable
span stack, a scratch buffer, and a diagnostic table, none of which are
locked. The intended model is one context per thread or per core
process. Sinks must not call back into the same context. The two
`obs_types.h` functions are re-entrant and thread-safe.

## Verification

`tests/unit/test_obs_types.c` pins (at compile time) the severity and
field-kind values, `COG_OBS_SEVERITY_COUNT`, `COG_OBS_SPAN_ID_NONE == 0`,
the exact field types of the value structs, and the include guard; at
run time it verifies severity names (all six, NULL out, invalid
severity with the out parameter unchanged) and the monotonic clock
(NULL out, non-decreasing readings).

`tests/unit/test_obs.c` pins (at compile time) the connection-state and
event-kind values, the exact field types of the sink, snapshot, and
event structs, and the include guard; at run time it verifies, with a
deterministic clock:

- lifecycle: NULL `out`/`config`, invalid severity, malformed
  fields/sinks tables, size-wrap `COG_ERROR_NOMEM`, default and custom
  capacities (exact boundary behaviour), `free(NULL)`;
- logging: NULL/severity/field validation, context filtering, exact
  record text for every field kind, string escaping, no-service
  records, sink threshold filtering, sink failure with remaining sinks
  still offered the record, construction rejection of a NULL
  `deliver` callback (fail closed), scratch overflow with no delivery;
- `log_to`: NULL-buffer rules, overflow with required-capacity
  reporting and successful retry, exact-fit boundary, filtered-out
  empty output, NULL `out_len`, sink text identical to the caller's
  buffer;
- `enabled`: NULL/invalid arguments and the threshold boundary;
- trace spans: begin/end round-trip (id, parent, name, start, duration,
  status, ok), nesting and parent linkage, current/depth, capacity
  overflow, NOT_FOUND and out-of-order INVALID_ARG with the stack
  unchanged, NULL sink, sink rejection with stack restore and
  successful retry, error-status records;
- diagnostics: publish insert/update, validation, table overflow with
  update-on-full still working, clear (found/not-found/zero-id/NULL),
  enumeration skipping gaps, counts, state counts, and the exact
  `diag_format` line (full, NULL-omitted, invalid state, overflow with
  required capacity);
- connection events: all seven kinds delivered with every field intact,
  no-sink no-op, NULL/invalid-kind validation, sink failure
  propagation.

`tests/unit/test_obs_critic.c` (Critic regression suite, ticket 84edd97)
adds: diag slot reuse after clear, update-in-place preserving slot
position, `trace_begin`/`trace_end` stack-unchanged on clock failure,
`trace_end` retry idempotence (no duplicate emission after close),
log-only `log_to` delivery, NULL `deliver` rejection at construction,
filtered-record field-validation precedence, and `conn_id == 0`
connection events. All three obs tests are registered in
`tests/unit/CMakeLists.txt` (`unit-obs`, `unit-obs-types`,
`unit-obs-critic`).

The tests build and pass under GCC and Clang with the project's strict
warning set (`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
-Wshadow -Wmissing-prototypes -Wstrict-prototypes -Wundef
-Wpointer-arith -Wcast-align -Wwrite-strings -Wformat=2 -Wlogical-op`)
and under AddressSanitizer + UndefinedBehaviorSanitizer.
