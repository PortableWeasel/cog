# Modules {#page_modules}

Cog is built from eight libraries plus the `cog-cli` application. Each
library owns one concern, exposes a single public header tree under
`include/cog/`, and depends only on the layers below it. The dependency graph
is acyclic:

```
cog-core ──▶ cog-ipc, cog-obs, cog-provider (forward declarations)
cog-provider, cog-toolkit ──▶ cog-core
cog-cli ──▶ everything
```

| Module | Umbrella header | Responsibility |
|--------|-----------------|----------------|
| @ref page_cog_core "cog-core" | `<cog/core.h>` | Agent loop, state machine, sessions, events, tools table, persistence |
| @ref page_cog_provider "cog-provider" | `<cog/provider.h>` | Endpoint/model registry and the provider vtable |
| @ref page_cog_toolkit "cog-toolkit" | `<cog/toolkit.h>` | Tool call contexts, execution, parallel runs, result collections |
| @ref page_cog_ipc "cog-ipc" | `<cog/ipc.h>` | Unix-domain-socket fabric, framing, session routing, replay |
| @ref page_cog_config "cog-config" | `<cog/config.h>` | Dotted-path configuration tree with candidate/commit/rollback |
| @ref page_cog_net "cog-net" | `<cog/net.h>` | Nonblocking HTTP client with retained chunked responses |
| @ref page_cog_obs "cog-obs" | `<cog/obs.h>` | Structured logging, trace spans, connection diagnostics |
| @ref page_cog_orchestrator "cog-orchestrator" | `<cog/orchestrator.h>` | Child-process lifecycle, health, placement, handoff |
| @ref page_cog_cli "cog-cli" | `<cog_cli/cli.h>` | The installed command-line application and its testable app core |

## How to read these pages

Each module page has the same shape:

1. **What it does** — the problem the module owns and the boundaries it keeps.
2. **Key ideas** — the small set of types and rules you must know.
3. **Getting started** — a complete, compilable example.
4. **Beyond the basics** — the parts you reach for once the simple case works.

All examples are single-threaded and caller-allocated, matching the project's
[core design constraints](@ref mainpage). They compile against the installed
headers with the strict warning set used by `tests/critic-smoke`:

```bash
python3 scripts/check-doc-examples.py
```

That script extracts every fenced `c` block that includes a Cog header and
compiles it with `-std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror`. Keep it
green when editing a page.

## Suggested order

New readers usually follow the life of one turn:

1. @ref page_cog_config "Configure" the endpoint and limits.
2. @ref page_cog_net "Fetch" over HTTP (or connect a provider directly).
3. @ref page_cog_provider "Talk" to a provider through the vtable.
4. @ref page_cog_core "Drive" the state machine and session.
5. @ref page_cog_toolkit "Run" tools the model asks for.
6. @ref page_cog_obs "Observe" the whole thing.
7. Run several cores with @ref page_cog_ipc "IPC" and
   @ref page_cog_orchestrator "the orchestrator".
8. Ship it through @ref page_cog_cli "cog-cli".

See also the [design blueprint](DESIGN.md) and the
[scenario traces](scenarios/01-user-prompt.md) that pin these flows down
end-to-end.
