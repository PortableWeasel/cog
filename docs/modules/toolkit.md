# cog-toolkit {#page_cog_toolkit}

## What it does

`cog-toolkit` turns a model's tool-call request into a controlled function
invocation. It owns:

- the **call context** (`cog_toolkit_call_t`) that carries a tool name, a
  copied request payload, a timeout, and cancellation state,
- **execution** (`cog_toolkit_execute` for one call, `cog_toolkit_execute_all`
  for a batch),
- **result validation** and **result collections**,
- the **workspace** abstraction for tools that read or write files.

Tool *descriptors* live in `cog-core`'s `cog_tool_t` / `cog_tool_table_t`;
this library looks them up and runs them.

Headers: `toolkit.h`, `toolkit_call.h`, `toolkit_exec.h`,
`toolkit_result.h`, `workspace.h`.

## Key ideas

**At-most-once execution.** A call context invokes its handler at most once.
Later executions return the cached outcome; a handler is never silently
retried after a partial effect. `side_effects_possible` records when a safe
replay is no longer guaranteed.

**Cancellation and timeouts are explicit.** `cog_toolkit_call_set_timeout`
sets a per-call deadline; `cog_toolkit_call_cancel` aborts unstarted work.
A cancelled call returns `COG_ERROR_ABORTED`.

**Capacity is passed, never guessed.** The caller supplies the output buffer
and its capacity. A result that does not fit reports `COG_ERROR_OVERFLOW`
with the required length instead of truncating silently.

**Parallelism is opt-in.** `max_parallel = 1` (the default) runs inline on
the caller's thread. Higher values spawn worker threads and require every
involved tool to be flagged reentrant and independent.

**Two execution styles.** The blocking API above is a compatibility path. The
asynchronous `cog_toolkit_execution_*` API reserves bounded state up front
and is what the responsive core uses for process and workspace tools.

## Getting started: one tool, one call

```c
#include <cog/core.h>
#include <cog/toolkit.h>

#include <stdio.h>
#include <string.h>

/* Echo tool: copies the request payload back as the result. */
static cog_result_t echo_tool(cog_core_t *core,
                              const char *request, size_t request_len,
                              char *result, size_t result_capacity,
                              size_t *out_result_len)
{
    (void)core;
    if (request_len >= result_capacity) {
        if (out_result_len != NULL) {
            *out_result_len = request_len;
        }
        return COG_ERROR_OVERFLOW;
    }
    memcpy(result, request, request_len);
    result[request_len] = '\0';
    if (out_result_len != NULL) {
        *out_result_len = request_len;
    }
    return COG_OK;
}

static const cog_tool_t tools[] = {
    {
        .name = "echo",
        .description = "Return the request payload unchanged",
        .handler = echo_tool,
    },
};

void run_one_tool(cog_core_t *core);

void run_one_tool(cog_core_t *core)
{
    cog_tool_table_t table = {tools, sizeof tools / sizeof tools[0]};
    cog_toolkit_call_t *call = NULL;
    cog_toolkit_result_t result = {0};
    char output[256];

    result.output = output;

    if (cog_toolkit_call_new(&call, "echo", "{\"text\":\"hi\"}", 13) != COG_OK) {
        return;
    }

    if (cog_toolkit_execute(core, &table, call, &result, sizeof output) == COG_OK) {
        printf("%s\n", result.output);
    } else {
        fprintf(stderr, "tool failed: %d\n", (int)result.status);
    }

    cog_toolkit_call_free(call);
}
```

The handler receives the exact bytes registered in the call. It writes at
most `result_capacity` bytes including the terminator and reports the true
length through `out_result_len`.

## Running a batch

`cog_toolkit_execute_all` takes an array of call contexts and returns a
collection. It blocks until every dispatched call finishes. Terminal results
are retained even when `stop_on_error` is set; calls that never started are
reported as `COG_ERROR_ABORTED`.

```c
#include <cog/core.h>
#include <cog/toolkit.h>

void run_batch(cog_core_t *core, const cog_tool_table_t *table,
               cog_toolkit_call_t *const *calls, size_t count);

void run_batch(cog_core_t *core, const cog_tool_table_t *table,
               cog_toolkit_call_t *const *calls, size_t count)
{
    cog_toolkit_exec_opts_t options = {0};
    cog_toolkit_result_collection_t *results = NULL;
    size_t i;

    options.timeout_ms = 5000;
    options.max_parallel = 4;
    options.stop_on_error = true;

    if (cog_toolkit_execute_all(core, table, calls, count,
                                &options, &results) != COG_OK) {
        return;
    }

    for (i = 0; i < cog_toolkit_result_collection_count(results); i++) {
        const cog_toolkit_result_t *result = NULL;

        if (cog_toolkit_result_collection_get(results, i, &result) == COG_OK &&
            cog_toolkit_result_is_ok(result)) {
            /* result->output is owned by the collection */
        }
    }

    cog_toolkit_result_collection_free(results);
}
```

Parallel dispatch requires every participating tool to carry
`COG_TOOL_REENTRANT | COG_TOOL_INDEPENDENT`. Otherwise the call fails before
any tool runs, so a partially-parallel batch is impossible.

## Beyond the basics

- **Tool flags.** `COG_TOOL_SIDE_EFFECTING` warns that a failure may not be
  safely replayed. The core uses these flags when admitting parallel work.
- **Async execution.** `cog_toolkit_execution_new` reserves output and
  workspace up front; `start` / `poll` / `cancel` / `result` / `free` let the
  core drive long-running tools without blocking the loop.
- **Workspaces.** `cog_workspace_new` gives file tools a bounded root and
  configuration. See `workspace.h`.
- **Schema validation.** `cog_toolkit_result_validate_schema` checks a
  result against the tool's declared output schema.

## See also

- @ref page_cog_core — the core calls handlers, the toolkit runs tools
- @ref page_cog_provider — where tool calls originate
- @ref page_cog_obs — tracing tool execution
