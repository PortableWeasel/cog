# Application scenario harness

Tests link `cog::cli` and drive the real application, core, session, provider
callbacks, toolkit and durable store. MOCK supplies deterministic provider
output; it does not replace tool execution or permit effects. The actual
adapter/net composition has an offline TLS test (`cli-app-https`).

Start every options/message/config value at zero. Programmatic options must
set presence bits; an endpoint string alone does not select the mock backend:

```c
cog_cli_options_t opts = {
    .backend = COG_CLI_BACKEND_MOCK,
    .explicit_options = COG_CLI_OPT_BACKEND,
    .prompt = "hello",
};
cog_cli_app_t *app = NULL;
CHECK(cog_cli_app_new(&app, &opts) == COG_OK);
CHECK(cog_cli_app_run(app) == COG_OK);
cog_cli_app_free(app);
```

Legacy `script` supplies CHUNK/ERROR/CANCEL/COMPLETE steps, consumed across sends.
An error followed by a chunk proves a retry only when the error precedes accepted
output. Two retryable errors exhaust the default single retry. Generic error
snapshots are valid until the next send; do not assert stale metadata after a
successful retry. Exact send/error counters remain observable.

For structured turns, use `opts.mock_deltas` and `mock_delta_count` (at most 4096
borrowed entries). They are MOCK-only and mutually exclusive with legacy script.
Keep all nested strings/deltas alive until app destruction. The app uses the
public `cog_mock_provider_new_deltas` constructor. Each fixture turn ends in a
FINISH. A complete tool turn is BEGIN, ARGUMENTS, END, FINISH/TOOL_CALLS; subsequent
TEXT + FINISH/STOP supplies the model continuation. Use strict built-in schemas.
`test_app_loop.c` and scenario15 are working full-round examples.

For effect tests, create a disposable `mkdtemp` workspace and durable store;
set WORKSPACE, STORE_MODE, STORE_DIR and POLICY presence bits. Explicit ALLOW is
preapproved only within those fixture roots. Assert the actual file/process
result and durable identified ledger, including the matching result in the next
provider request. DENY and no-store tests assert absence of the effect. Never
use a legacy synchronous tool as a shortcut around durable async admission.

| Injection | Contract |
|---|---|
| `app_submit` | Copies UTF-8 text into a bounded slot through handler completion |
| `app_queue` | Retains copied input for a later safe turn; cannot authorize failed-turn continuation |
| `app_cancel` | Enqueues owner-thread cancellation and retains explicit cancelled display state |
| `app_shutdown` | Stops admission, cancels/drains/reaps, saves outcomes, then stops |
| `app_stop` | Only cross-thread/signal-safe operation; run drains after core exits |

One completed assistant message contains all text chunks. Inspect complete
provider history with `cog_session_copy_history`; incomplete or unresolved
history must refuse replay. Use `app_display_snapshot` to inspect cancelled,
partial or unresolved visible state. The application snapshot never exposes
opaque continuation. For retained failed branches use `cog_session_snapshot_view`
and verify the old node still exists after a freshly authorized continuation.
Prequeued input must not cause that continuation automatically.

Failure-injection tests wrap allocation, user/STARTED/result saves and TOOL
insertion. They verify zero effects before durable STARTED, a retained terminal
result after recording failure, no second provider send and immediate freeze
following uncertain persistence. Tool tests also cover schema refusal, finite
round/turn deadlines, explicit truncation, sanitized command environment,
cancellation and child reap. Every temporary directory has a unique prefix;
cleanup only operates on its owned root/PIDs.

The scoped K selection is:

```sh
ctest --test-dir build-k \
  -R '^(cli-|scenario-test_(01|02|04|05|06|12|15|16|17|20))' \
  --output-on-failure
```

This selects 25 checks with the audited transport profile. Original03/07/09/10
are broader agent/IPC/orchestrator scenarios; app fixture migrations preserve
those assertions while O supplies their remaining runtime composition. The full
release gate still runs every original scenario plus R01–R18; this selection is
not that gate. Sanitizers use separate build directories. The focused TSan run
covers `cli-app-stop` and `cli-app-limits` with real cross-thread stop calls.
