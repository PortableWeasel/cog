# cog-obs {#page_cog_obs}

## What it does

`cog-obs` is the observability layer. It provides:

- **structured logging** with severity filtering and typed fields,
- **trace spans** for nested operations,
- **connection diagnostics** — a bounded table of live connection snapshots,
- **connection events** that a UI can render as they happen.

It never opens, closes, or flushes a destination. A sink is a callback that
receives already-formatted text and a handle you own.

Headers: `obs.h`, `obs_types.h`.

## Key ideas

**Sinks are static and borrowed.** A `cog_obs_sink_table_t` points at an
array of `cog_obs_sink_t` descriptors. Each sink has a `deliver` callback, a
`user` handle, and a minimum severity. The table must outlive the context.

**Field indices, not names, at call time.** You declare a static field table
once (`cog_obs_field_desc_t` rows with a name and a kind). A log call then
passes `cog_obs_field_t` values that reference a descriptor by index and carry
a matching value. This keeps formatting cheap and catches kind mismatches.

**Severity is filtered before formatting.** If the record or the sink is more
verbose than its threshold, nothing is formatted or delivered.

**Tracing is always available.** Spans record timestamps and a stack of ids.
If no `trace_sink` is configured, span records are simply discarded. There is
no compile-time switch and no allocation on the span path.

**Connection state is bounded.** The diagnostic table has a fixed capacity
set at construction. Publishing a snapshot for a connection id replaces its
slot; the table never grows.

## Getting started: log to stdout with one field

```c
#include <cog/obs.h>

#include <stdio.h>

static cog_result_t to_stdout(void *user, const char *record, size_t length)
{
    (void)user;
    if (fwrite(record, 1, length, stdout) != length) {
        return COG_ERROR_IO;
    }
    if (fputc('\n', stdout) == EOF) {
        return COG_ERROR_IO;
    }
    return COG_OK;
}

static const cog_obs_sink_t sinks[] = {
    {
        .name = "stdout",
        .deliver = to_stdout,
        .user = NULL,
        .min_severity = COG_OBS_INFO,
    },
};

static const cog_obs_field_desc_t fields[] = {
    { .name = "request_id", .kind = COG_OBS_FIELD_UINT },
};

static const cog_obs_field_table_t field_table = { fields, 1 };
static const cog_obs_sink_table_t sink_table = { sinks, 1 };

int main(void)
{
    cog_obs_config_t config = {0};
    cog_obs_t *obs = NULL;
    cog_obs_field_t field = {0};

    config.min_severity = COG_OBS_TRACE;
    config.fields = &field_table;
    config.sinks = &sink_table;
    config.service = "demo";

    if (cog_obs_new(&obs, &config) != COG_OK) {
        return 1;
    }

    field.index = 0;                  /* the only declared field */
    field.kind = COG_OBS_FIELD_UINT;  /* must match the descriptor */
    field.value.unsigned_integer = 42;

    (void)cog_obs_log(obs, COG_OBS_INFO, "turn started", &field, 1);

    cog_obs_free(obs);
    return 0;
}
```

`cog_obs_log_to` is the same call with an extra output buffer, so a caller
(for example the CLI) can display exactly the text it logged.

## Trace spans

```c
#include <cog/obs.h>

cog_result_t timed_operation(cog_obs_t *obs, cog_obs_span_id_t *out_id);

cog_result_t timed_operation(cog_obs_t *obs, cog_obs_span_id_t *out_id)
{
    cog_obs_span_id_t id = 0;
    cog_result_t work = COG_OK;
    cog_result_t rc;

    rc = cog_obs_trace_begin(obs, "provider.send", &id);
    if (rc != COG_OK) {
        return rc;
    }

    /* ... do the work, recording its outcome in `work` ... */

    rc = cog_obs_trace_end(obs, id, work);
    if (out_id != NULL) {
        *out_id = id;
    }
    return rc;
}
```

`cog_obs_trace_current` and `cog_obs_trace_depth` expose the current span
stack. Exceeding `span_capacity` makes `trace_begin` return
`COG_ERROR_OVERFLOW` rather than growing, so a runaway recursion is visible
instead of silent.

## Connection diagnostics

The diagnostic table is designed for a UI that shows live connections:

- `cog_obs_diag_publish(obs, &snapshot)` stores or replaces one connection,
- `cog_obs_diag_at(obs, index, &snapshot)` reads one back,
- `cog_obs_diag_count` / `cog_obs_diag_count_state` summarize the table,
- `cog_obs_diag_clear(obs, conn_id)` removes one entry,
- `cog_obs_diag_format` renders a human-readable line.

A `cog_obs_conn_snapshot_t` carries endpoint, provider, state, timestamps,
byte counters, retry count, and last status. Connection events are delivered
to `conn_event_sink` as they are emitted, which is the push channel; the table
is the pull channel for late-joining UIs.

## Beyond the basics

- **Deterministic clocks.** `cog_obs_config_t.clock_ns` can substitute a fake
  clock, which is how tests make timestamps reproducible.
- **Severity names.** `cog_obs_severity_name` renders a stable string for a
  level; `cog_obs_clock_now_ns` is the default monotonic clock.
- **Why sinks do not own files.** Ownership stays with the application, so
  logging can never close a descriptor the application still uses.

## See also

- @ref page_cog_core — logs lifecycle events through this context
- @ref page_cog_net — where connection snapshots come from
- `applications/cog-cli/tests/test_18_observability.c`
