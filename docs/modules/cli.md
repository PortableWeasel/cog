# cog-cli {#page_cog_cli}

## What it does

`cog-cli` is the installed command-line application and the testable library
behind it. It wires the other modules into one executable:

- parses options and resolves configuration (CLI > environment > file >
  defaults),
- builds the session, store, provider, transport, observation context, and
  core,
- runs the interactive or one-shot conversation,
- owns the durable session lifecycle: new, list, resume, fork, and recovery.

Headers: `cog_cli/cli.h` and `cog_cli/mock_provider.h`.

## Key ideas

**Two backends, no silent fallback.** `COG_CLI_BACKEND_REAL` uses the
OpenRouter adapter and a caller-owned `cog_net_ctx_t`. `COG_CLI_BACKEND_MOCK`
uses the deterministic fixture provider. A real setup that fails never falls
back to mock.

**The host owns the dependency lifecycle.** `main.c` calls
`curl_global_init` before creating the transport, and `curl_global_cleanup`
after freeing the app and the transport. The app never initialises global
state.

**Options carry explicit presence bits.** A zeroed `cog_cli_options_t` means
"use defaults". `cog_cli_options_parse` fills defaults and marks which flags
were actually supplied in `explicit_options`, so a programmatic caller can
distinguish "unset" from "explicitly zero".

**Durability is explicit.** Real mode defaults to a durable store; mock mode
defaults to no store. Resume, fork, and recovery require a durable store.

**Metadata actions are offline.** `--sessions` (list) and recovery-only mode
construct no provider, credential, or transport, so they work without a
network or secrets.

## Getting started: run the installed binary

```bash
# One-shot turn against the deterministic mock endpoint.
./build/applications/cog-cli/cog-cli --prompt hello --mock

# Stream several chunks, then queue a follow-up message.
./build/applications/cog-cli/cog-cli --prompt hello --mock \
  --chunks first,second -q 'next turn'

# Interactive mode (reads prompts from stdin).
./build/applications/cog-cli/cog-cli --mock

# List durable sessions, then resume one.
./build/applications/cog-cli/cog-cli --sessions --store ~/.local/state/cog
./build/applications/cog-cli/cog-cli --resume <session-id> --store ~/.local/state/cog
```

The default mock script answers `hello` without any network service or
credentials. `--mock` is required for the fixture; real mode is the default
and needs a model, an endpoint, and a credential reference.

## Getting started: embed the app

The library entry points are small enough to drive from your own `main`.
This example runs the mock backend without touching the argument parser's
real-mode dependencies.

```c
#include <cog_cli/cli.h>

int main(int argc, char **argv)
{
    cog_cli_options_t options;
    cog_cli_app_t *app = NULL;
    cog_result_t rc;
    int status = 0;

    rc = cog_cli_options_parse(&options, argc, argv, &status);
    if (rc == COG_ERROR_ABORTED) {
        return 0; /* --help / --version */
    }
    if (rc != COG_OK) {
        return 2;
    }

    /* Force the deterministic fixture backend for a self-contained demo. */
    options.backend = COG_CLI_BACKEND_MOCK;
    options.explicit_options |= COG_CLI_OPT_BACKEND;

    if (cog_cli_app_new(&app, &options) != COG_OK) {
        return 1;
    }

    rc = cog_cli_app_run(app);
    cog_cli_app_free(app);
    return rc == COG_OK ? 0 : 1;
}
```

For real mode you would instead create a `cog_net_ctx_t` (see
@ref page_cog_net "cog-net"), assign it to `options.net`, and leave the
backend at its default.

## A scripted mock turn

`cog_cli/mock_provider.h` exposes the fixture directly when you want precise
control over a turn. A script is an array of steps: chunks, a retryable
error, a cancellation, or completion.

```c
#include <cog_cli/mock_provider.h>

#include <stdio.h>

static const cog_mock_step_t steps[] = {
    { .kind = COG_MOCK_STEP_CHUNK, .chunk = "hello" },
    { .kind = COG_MOCK_STEP_CHUNK, .chunk = ", world" },
    { .kind = COG_MOCK_STEP_COMPLETE },
};

int main(void)
{
    cog_mock_script_t script = {0};
    cog_provider_t *provider = NULL;

    script.steps = steps;
    script.step_count = sizeof steps / sizeof steps[0];

    if (cog_mock_provider_new(&script, &provider) != COG_OK) {
        return 1;
    }

    printf("mock provider ready\n");
    (void)cog_provider_disconnect(provider);
    return 0;
}
```

This is the same fixture the CLI uses for `--chunks` and
scenario tests; it never sleeps and never allocates during a turn.

## Session commands

| Flag | Action |
|------|--------|
| `--sessions` | List durable session ids (offline). |
| `--resume <id>` | Restore a committed session and continue it. |
| `--fork <id>` | Restore, then select a safe node for fresh input. |
| `--from-node <n>` | Fork point for `--fork`; default is the active leaf. |
| `--recover <id>` | Recovery-only view of uncertain history; never executes. |
| `--fenced` | Assert prior writers are quiescent (required for recovery/adoption). |
| `--store <dir>` | Durable store directory. |
| `--no-store` | Disable persistence (text-only conversation). |

Recovery is deliberately not automatic. If a session was interrupted with an
in-flight tool call, the CLI refuses ordinary continuation and requires an
operator decision, because Cog cannot prove whether the effect happened.

## Beyond the basics

- **Tool policy.** `--tool-policy ask|deny|allow` controls execution. `ask`
  prompts interactively and denies in noninteractive runs. `allow` is an
  explicit autonomous opt-in but still validates paths and limits.
- **Limits.** Output tokens, tool rounds, turn time, tool time, tool output
  bytes, context bytes, retries, and retry elapsed time are all explicit
  options with finite defaults. Exhausting them returns a typed error rather
  than silently truncating.
- **Display snapshots.** `cog_cli_app_display_snapshot` and its encode/decode
  helpers let a UI process render the same state a test observes.
- **Graceful stop.** `cog_cli_app_stop` is safe from a signal context; the
  app drains and reaps before `cog_cli_app_free`.

## See also

- `applications/cog-cli/CONTRACT.md` — the full option and ownership contract
- `applications/cog-cli/CONFIGURATION.md` — configuration keys
- `applications/cog-cli/HARNESS.md` — fixture APIs for scenario tests
- @ref page_cog_core, @ref page_cog_net, @ref page_cog_provider
