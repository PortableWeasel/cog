# Scenario Harness Contract

This document is the contract for the four scenario batches that follow
this ticket:

| Ticket | Scenarios |
|--------|-----------|
| `fe958ce` (12.6b) | 01 user-prompt, 02 network-recovery, 03 subagent-lifecycle, 04 cold-start, 05 graceful-shutdown |
| `0d8ff50` (12.6c) | 06 parallel-tools, 07 aux-crash, 08 config-reload, 09 subagent-messaging, 10 fan-out-fan-in |
| `e69a38a` (12.6d) | 11 ui-reconnect, 12 rate-limit, 13 session-persistence, 14 conversation-fork, 15 multi-turn-tools |
| `c12c392` (12.6e) | 16 error-propagation, 17 session-migration, 18 observability, 19 connection-events, 20 interrupt-and-queue |

Scenario tests link `cog::cli` and drive the app **programmatically** —
they never spawn the binary.  `tests/test_cli_harness.c` is a working
template: copy a scenario function from it and extend.

## The fixture pattern

Every scenario follows the same five steps:

```c
/* 1. Script the mock provider (chunks / errors / rate limits / cancel). */
static const cog_mock_step_t steps[] = {
    {COG_MOCK_STEP_CHUNK, "The", COG_OK, 0, false, 0U, NULL},
    {COG_MOCK_STEP_CHUNK, " answer", COG_OK, 0, false, 0U, NULL},
};
static const cog_mock_script_t script = { steps, 2U, 0U, COG_OK, 0U };

/* 2. Build options and create the app. */
cog_cli_options_t opts;
cog_cli_app_t *app = NULL;
memset(&opts, 0, sizeof(opts));
opts.prompt = "What is the answer?";   /* or leave NULL and submit manually */
opts.script = &script;                 /* overrides chunks/mock_mode */
opts.model_id = "mock-model";          /* defaults parse would supply */
opts.endpoint_name = "mock";
opts.interactive = false;              /* single-prompt mode */
CHECK(cog_cli_app_new(&app, &opts) == COG_OK);

/* 3. Inject prompts (optional; opts.prompt is submitted by run). */
CHECK(cog_cli_app_submit(app, "extra prompt", 12U) == COG_OK);

/* 4. Run. */
CHECK(cog_cli_app_run(app) == COG_OK);

/* 5. Observe. */
const cog_message_t *msgs = NULL; size_t n = 0;
cog_session_get_history(cog_cli_app_session(app),
                        cog_session_active(cog_cli_app_session(app)),
                        &msgs, &n);
cog_mock_state_t st;
cog_mock_provider_state(cog_cli_app_provider(app), &st);
```

## Injecting input

| API | Purpose |
|-----|---------|
| `opts.prompt` | one prompt submitted automatically by `cog_cli_app_run` |
| `cog_cli_app_submit(app, text, len)` | enqueue a `COG_EVENT_USER_INPUT` (payload borrowed; keep `text` alive until dispatch) |
| `cog_cli_app_queue(app, text, len)` | queue a message for the turn **after** the current one (core auto-injects it) |
| `cog_cli_app_cancel(app)` | enqueue `COG_EVENT_CANCEL` (arms provider cancellation while streaming) |
| `cog_cli_app_shutdown(app)` | enqueue `COG_EVENT_SHUTDOWN` (graceful stop) |
| `cog_cli_app_stop(app)` | signal-safe immediate stop request |

Ordering matters for cancellation: submit the prompt **before** the
cancel so the `CANCEL` event is dispatched while the turn is in
`AWAITING_LLM` (see `scenario_cancel` in `test_cli_harness.c`).

## Scripting the provider

| Field | Effect |
|-------|--------|
| `COG_MOCK_STEP_CHUNK` | emit `chunk` as a streaming delta |
| `COG_MOCK_STEP_ERROR` | receive fails with `error_code` + `http_status` / `retryable` / `retry_after_ms` / `error_message` |
| `COG_MOCK_STEP_CANCEL` | receive returns `COG_ERROR_ABORTED` |
| `COG_MOCK_STEP_COMPLETE` | receive returns an empty STOP completion |
| `script.repeat` | replay the script for multi-turn sessions (`SIZE_MAX` = forever) |
| `script.send_error` | make every send fail with this code |
| `script.chunk_delay_ms` | simulated latency between chunks |

Steps are consumed monotonically across the provider's lifetime, so
`[ERROR(429, retryable), CHUNK("recovered")]` makes the first receive
fail and the retry's first receive succeed (scenario 12).  The CLI
retries retryable errors once with a bounded backoff.

For a scripted provider-side abort, use a `CANCEL` step; for a
user-side cancel, call `cog_mock_provider_request_cancel(provider)`
directly or `cog_cli_app_cancel(app)` while streaming.

## Observing state

| API | What it reveals |
|-----|-----------------|
| `cog_session_get_history(session, active, &msgs, &n)` | conversation tree: roles, content, lengths |
| `cog_session_active(session)` / `cog_node_data(node)` | current leaf node and its metadata |
| `cog_mock_provider_state(provider, &st)` | `send_count`, `receive_count`, `chunk_count`, `error_count`, `cancel_count`, `last_request` (struct copy), `last_error` (retry metadata) |
| `cog_cli_app_state(app)` / `cog_core_state(core)` | current state-machine state (0 = IDLE, 1 = AWAITING_LLM) |
| `cog_cli_app_obs(app)` + `cog_obs_diag_*` | connection diagnostics table (scenario 19) |
| `cog_cli_app_store(app)` + `cog_store_*` | persistence round-trips (scenario 13) |
| `cog_cli_app_core(app)` | the core, for `cog_core_*` accessors and `cog_agent_*` subagent APIs (scenarios 03, 09, 10) |
| `cog_core_user(cog_cli_app_core(app))` | the app pointer threaded through the core's borrowed user context (handlers reach the app this way; no global) |

## Scenario notes

- **01 user-prompt**: stream chunks, assert history = user + N assistant
  nodes and `st.chunk_count == N`.
- **02 network-recovery / 12 rate-limit**: script `ERROR` with
  `retryable = true` and `retry_after_ms`; assert `st.error_count == 1`,
  `st.send_count == 2`, and the recovered chunk in history.
- **05 graceful-shutdown**: `cog_cli_app_shutdown` then run; expect
  `COG_OK` and a clean teardown (ASan-clean).
- **13 session-persistence**: set `opts.store_dir`; after the run assert
  `cog_store_exists(store, cog_session_id(session))`.
- **20 interrupt-and-queue**: path A = submit then cancel, assert only
  the user message remains; path B = `cog_cli_app_queue` while a turn is
  pending, assert both turns appear in history.

## Ownership and teardown

The app owns everything it creates; `cog_cli_app_free(app)` releases it
in the documented order (core → provider → obs → store → session →
script storage).  Scripts and their strings are borrowed by the provider
(struct copied at connect), so keep them alive until the app is freed.
