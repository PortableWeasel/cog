# cog-cli — Application Contract

Stage 14.J supersedes the baseline options/configuration portions below with
[effective configuration](CONFIGURATION.md). The independent `cog-cli-config`
static library implements parsing and private immutable generation construction;
`cog::cli` propagates it plus toolkit/net. H/K still migrate provider dispatch and
the full application loop. These baseline scenario claims are not a Stage 14
real-client release pass.

`cog-cli` is the command-line application for Cog: a single-process LLM
agent harness that wires the real `cog-core`, `cog-provider`, `cog-obs`,
and `cog-store` libraries into a functional prompt flow.  It is the
foundation for the executable scenario tests (Stages 12.6b–12.6e).

## Layout

```
applications/cog-cli/
  include/cog_cli/
    cli.h             public app API: options, lifecycle, event injection, accessors
    mock_provider.h   deterministic scripted mock provider + observable state
  src/
    main.c            thin entry point: parse, signal handlers, run, exit code
    cli.c             app implementation: dispatch table, handlers, wait hook
    mock_provider.c   mock provider implementation (no network)
  tests/              cli-* tests and executable scenarios 11–20
  CONTRACT.md         this file
  HARNESS.md          scenario-writer guide (fixtures and observation)
```

Scenarios 01–10 and the 05b shutdown regression live in
`tests/scenarios/`. All 20 numbered scenarios plus 05b use the shared
`scenario` CTest label.

## Build targets

| Target | Kind | Contents |
|--------|------|----------|
| `cog-cli-lib` (alias `cog::cli`) | static library | `cli.c`, `mock_provider.c` |
| `cog-cli` | executable | `main.c` linked against `cog::cli` |

`cog::cli` links `cog::core`, `cog::provider`, and `cog::obs` PUBLIC so
consumers (scenario tests) get the full include path and link closure.

## Installation and exports

```bash
cmake --install build --prefix install/stage
install/stage/bin/cog-cli --prompt hello
```

The install includes `bin/cog-cli`, `libcog-cli-lib.a`, the `cog_cli`
headers, and `cog-cli-targets.cmake`, which exports the library as
`cog::cli`. Load installed target sets in dependency order: `cog-result`,
`cog-ipc`, `cog-obs`, `cog-core`, `cog-provider`, then `cog-cli`.

## CLI options

```
-p, --prompt TEXT     run one prompt, print the response, and exit
-i, --interactive     read prompts from stdin (default)
-s, --session ID      session identifier (default: generated)
-d, --store DIR       persist sessions under DIR (auto-save after each turn)
-l, --log PATH        structured logs to PATH (default: stderr)
-S, --socket PATH     listen on a Unix domain socket (optional IPC)
-m, --model ID        model identifier sent in requests (default: mock-model)
-e, --endpoint NAME   endpoint name (default: mock; only "mock" exists)
-M, --mock-mode MODE  stream|error|cancel|rate-limit (default: stream)
-c, --chunks LIST     comma-separated mock chunks (default: "Hello from cog-cli!")
-q, --queue TEXT      queue TEXT for the turn after the current one
-v, --verbose         debug logging
-h, --help / -V, --version
```

Exit codes: `0` success (including `--help`/`--version`), `1` startup or
run failure, `2` bad arguments.

## State machine

The dispatch table is `static const` in `cli.c`:

| State | Event | Handler behaviour |
|-------|-------|-------------------|
| IDLE | `USER_INPUT` | append user message, send request from session history, start streaming |
| IDLE | `CANCEL` | no-op (self transition) |
| IDLE | `SHUTDOWN` | stop the run loop |
| AWAITING_LLM | `LLM_CHUNK` | receive next chunk, append + print, continue or finish |
| AWAITING_LLM | `LLM_COMPLETE` | close the turn, return to IDLE |
| AWAITING_LLM | `CANCEL` | arm provider cancellation; next receive aborts and the partial turn is discarded |
| AWAITING_LLM | `SHUTDOWN` | cancel the in-flight request (arm provider cancellation, drain the abort), discard the partial assistant turn, return to IDLE, stop the run loop |

Error handling: a retryable provider error (e.g. rate limit) is retried
once after a bounded backoff derived from the error's `retry_after_ms`
(default 100 ms, capped at 5 s); a non-retryable error propagates out of
`cog_core_run` and the process exits non-zero (fail loudly).

## Ownership

`cog_cli_app_t` owns the session, provider, obs context, store, mock
script storage, and (until hand-off) the IPC listener.  The core borrows
session/provider/obs/store and owns the IPC endpoint once configured.
Teardown order in `cog_cli_app_free`: core → provider → obs → store →
session → IPC (only if `cog_core_new` failed) → owned script storage →
log file.

The handlers need the app context (output stream, options) but the
handler signature only receives the core.  The app is threaded through
the core's borrowed `cog_core_config_t::user` pointer at construction
and retrieved in handlers with `cog_core_user(core)` (see
`cli_app_from_core` in cli.c).  The core never owns or mutates the
pointer, and the dispatch table itself remains static const — there is
no global mutable state.

## Mock provider

`mock_provider.h` implements the full `cog_provider_vtable_t` contract
with no network access.  A script is an ordered array of steps
(`CHUNK`, `ERROR`, `CANCEL`, `COMPLETE`); steps are consumed
monotonically, and the `repeat` field replays the script for multi-turn
sessions.  Cancellation is available both as a scripted step and as a
one-shot `cog_mock_provider_request_cancel`.  `cog_mock_provider_state`
snapshots send/receive/chunk/error/cancel counters, the last request
(struct copy; pointer fields borrowed), and the last error metadata.

The mock is deliberately **not** part of `cog-provider`: production
provider logic stays free of test scaffolding.

## Threading

Single-threaded.  `cog_cli_app_stop` is the only call safe from a signal
handler or another thread; it forwards to `cog_core_stop`.  The
interactive wait hook polls stdin with a 100 ms timeout so a stop
request is honoured promptly.

## Tests

The app-focused CTest names are `cli-options`, `cli-mock-provider`,
`cli-smoke`, `cli-harness`, `cli-regression`, and `cli-binary-smoke` (see
`tests/CMakeLists.txt`). The binary smoke test spawns the built `cog-cli`
executable and checks its stdout and exit code for the stream, error-retry,
cancel, custom-chunk, help, and bad-option cases.

Scenario tests use names `scenario-test_11_ui_reconnect` through
`scenario-test_20_interrupt_and_queue` in this directory and
`scenario-test_01_user_prompt` through `scenario-test_10_fan_out_fan_in` in
the shared scenario directory; `scenario-test_05b_shutdown_cancel_contract`
is also labeled `regression`. The complete project suite has 61 tests:

```bash
ctest --test-dir build --output-on-failure -j4
ctest --test-dir build -L scenario --output-on-failure
```
