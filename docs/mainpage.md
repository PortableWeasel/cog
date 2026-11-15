# Cog 0.1.0 {#mainpage}

An LLM harness and agent framework in C11: a table-driven state machine that
routes events (user input, tool results, signals) to handlers (provider calls,
tool execution, file I/O), validates results, and advances session state.
Think `pi` / `opencode`, but compiled and embeddable.

## Libraries

Each module has a guide with a high-level introduction and a compilable
getting-started example. Start at @ref page_modules.

| Library | Header | Guide | Purpose |
|---------|--------|-------|---------|
| cog-core | @ref core.h | @ref page_cog_core | Agent loop, session tree, event queue, state machine |
| cog-provider | @ref provider.h | @ref page_cog_provider | LLM endpoint registry, provider vtable, model/capability |
| cog-toolkit | @ref toolkit.h | @ref page_cog_toolkit | Tool call/exec/result API |
| cog-ipc | @ref ipc.h | @ref page_cog_ipc | Unix domain socket fabric, session routing, broadcast |
| cog-config | @ref config.h | @ref page_cog_config | Hierarchical config tree with commit/rollback |
| cog-net | @ref net.h | @ref page_cog_net | HTTP client, streaming, stall detection, retry |
| cog-obs | @ref obs.h | @ref page_cog_obs | Logging, tracing, diagnostics |
| cog-orchestrator | @ref orchestrator.h | @ref page_cog_orchestrator | Process lifecycle, health, placement |
| cog-cli | @ref cli.h | @ref page_cog_cli | Command-line application and testable app core |

## Design constraints (non-negotiable)

1. **The interface is the truth.** Public headers in `include/cog/` define the
   contract. Implementation details go in `src/` and stay private.
2. **Static const dispatch tables.** `(state, event) -> handler` and the
   provider/toolkit/store tables are `static const` data: indexer-friendly,
   no dynamic registration.
3. **No global mutable state.** Every context struct is caller-allocated and
   passed by pointer.
4. **Zero global constructors.** No `__attribute__((constructor))`, no implicit
   init. If the user doesn't call `cog_init()`, nothing happens.
5. **Every allocation is visible.** The library never mallocs behind the
   caller's back. Buffers are caller-provided, or returned by a constructor
   with a matching destructor. Output buffers take an explicit capacity
   parameter.
6. **Fail closed, fail loudly.** Every API returns an error code
   (`cog/result.h`). Assert internal invariants in debug builds; never
   swallow an unexpected state.
7. **Single-threaded first.** One agent loop, one thread. @ref cog_core_stop
   is the only call intended to be safe from another thread or a signal
   handler.

## API reference

- @ref result.h "Result codes" — shared error codes
- @ref core.h "Core" — agent lifecycle, state machine
- @ref session.h "Session" — conversation tree, forking
- @ref event.h "Event" — event kinds, event queue
- @ref state.h "State" — state machine definition
- @ref handler.h "Handler" — handler registration
- @ref agent.h "Agent" — subagent metadata
- @ref tool.h "Tool" — tool invocation
- @ref message.h "Message" — message representation
- @ref session_store.h "Session store" — persistence backend
- @ref provider.h "Provider" — provider interface
- @ref provider_vtable.h "Provider vtable" — provider dispatch
- @ref endpoint.h "Endpoint" — endpoint configuration
- @ref provider_request.h "Provider request" — request payloads
- @ref openrouter.h "OpenRouter" — OpenRouter integration
- @ref ipc.h "IPC" — inter-process communication
- @ref toolkit.h "Toolkit" — tool registry
- @ref toolkit_call.h "Toolkit call" — tool call representation
- @ref toolkit_exec.h "Toolkit execution" — tool executor
- @ref toolkit_result.h "Toolkit result" — tool result data
- @ref workspace.h "Workspace" — file workspace
- @ref config.h "Config" — configuration tree
- @ref net.h "Network" — HTTP client
- @ref obs.h "Observability" — logging, tracing
- @ref obs_types.h "Observability types" — severity, categories
- @ref orchestrator.h "Orchestrator" — process management
- @ref cli.h "CLI" — command-line interface
- @ref mock_provider.h "Mock provider" — test fixture

## Scenarios

The project maintains 20 end-to-end scenario traces covering the full
lifecycle. See the [scenario pages](scenarios/01-user-prompt.md) for details.

## System design

- [Design blueprint](DESIGN.md) — system architecture
- [Implementation strategy](IMPLEMENTATION_STRATEGY.md) — build plan
- [Work breakdown](IMPLEMENTATION_WBS.md) — task tracking
- [Walkthrough](WALKTHROUGH.md) — code tour