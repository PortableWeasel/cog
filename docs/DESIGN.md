# Cog — System Design Blueprint

This document captures the software blueprint for Cog, an LLM harness and
agent framework written in C. It follows the Jeep Method: the fewest
essential parts that still make a working, maintainable system.

---

## Stage 1 — Goals

What the system must do:

- Receive events from user input, tool results, system signals, and UI
  connections.
- Maintain session context across the lifetime of a conversation.
- Route each event through a table-driven state machine to the correct
  handler.
- Dispatch events to handlers for LLM provider calls, tool execution, and
  file I/O.
- Validate handler results before advancing the session state.
- Track in-flight network connections, expose streaming progress, detect
  stalls, and recover from poor networking.
- Expose a public C API where every context is caller-allocated and every
  allocation is visible.
- Support pluggable provider and toolkit libraries via static dispatch
  tables.
- Run correctly in a single-process, single-threaded agent loop by default.
- Support first-class multi-process operation with a well-defined core/UI
  boundary.
- Allow the core to run headless and accept intermittent UI connections.
- Provide contract-based endpoints that external UIs can use to build rich
  clients.
- Register and manage LLM provider endpoints with configurable base URLs,
  API versions, credentials, and model identifiers.
- Track provider capabilities and constraints so the system can select the
  right model for a request.
- Select the active provider and model for a session or request based on
  configuration.
- Accept configuration through a hierarchical path-based model.
- Support candidate configuration changes that can be reviewed before
  applying.
- Validate and atomically commit configuration changes.
- Roll back to a previous known-good configuration on failure or request.

---

## Stage 2 — Functional blocks

The abstract subsystems the project needs:

- **Event ingestion** — accept user input, tool results, system signals,
  and messages from local or remote UI processes.
- **Session state** — hold conversation context, track current state, and
  survive across events and reconnections.
- **State machine & routing** — table-driven dispatch from `(state, event)`
  to the handler that should run next.
- **Processing / handlers** — the core logic: LLM provider calls, tool
  execution, file I/O, and result validation.
- **Networking** — HTTP client, connection lifecycle tracking, streaming
  response handling, stall detection, and retry/recovery.
- **Inter-process communication** — multi-process support, core/UI
  separation, and contract-based endpoints for external UIs.
- **Output / rendering** — send responses, streaming deltas, status
  updates, and diagnostics back to UI or log.
- **Configuration** — hierarchical config tree with path-based
  get/set/delete, candidate vs. running state, atomic commit with
  validation, and rollback.
- **Observability** — structured logging, tracing, and diagnostics
  including in-flight connection status.
- **Lifecycle** — explicit init/run/shutdown with no global constructors.
- **Provider & model registry** — store endpoint configurations, API
  versions, authentication, model identifiers, and capability metadata;
  resolve which provider/model to use for each request.

---

## Stage 3 — Goals cross-check

Mapping each goal to the blocks that serve it:

| Goal | Served by |
|------|-----------|
| Receive events from user input, tool results, signals, UI | Event ingestion |
| Maintain session context | Session state |
| Route events through state machine | State machine & routing |
| Dispatch to handlers | State machine & routing + Processing / handlers |
| Validate handler results | Processing / handlers |
| Track in-flight connections, stalls, recovery | Networking + Observability |
| Public C API, visible allocations | Lifecycle (API contract) + all blocks |
| Pluggable provider/toolkit via static dispatch tables | State machine & routing + Processing / handlers |
| Single-process, single-threaded loop by default | Lifecycle + State machine & routing |
| First-class multi-process | Inter-process communication |
| Headless, intermittent UI | Inter-process communication + Output / rendering + Session state |
| Contract-based endpoints for external UIs | Inter-process communication |
| Manage provider endpoints, versions, URLs, credentials | Provider & model registry + Configuration |
| Select provider/model per request | Provider & model registry + State machine & routing |
| Hierarchical path-based config | Configuration |
| Candidate/commit/rollback config semantics | Configuration |
| Accept configuration via CLI/env/config files | Configuration |

Every block serves at least one goal; every goal maps to at least one
block.

### Simplest viable version of each block

- **Event ingestion** — stdin reader, internal event queue for tool/signal
  messages, and an IPC message receiver.
- **Session state** — in-memory context struct per session; persistence
  deferred.
- **State machine & routing** — static const `(state, event) → handler`
  table.
- **Processing / handlers** — provider call, tool execution, and file I/O
  handlers that return validated results.
- **Networking** — HTTP client with request IDs, connection tracking,
  streaming response handling, timeout/stall detection, and simple retry.
- **Inter-process communication** — local socket or pipe transport with
  message framing and a clear core/UI boundary.
- **Output / rendering** — stream responses and status updates to
  stdout/IPC, with buffering for intermittent UI connections.
- **Configuration** — a tree of key-value nodes addressable by dotted
  paths (e.g. `providers.openai.base-url`). The caller makes changes to a
  candidate tree, then calls `commit()`. Commit runs validators registered
  per subtree; on failure, the candidate is discarded or rolled back. The
  running tree is what the rest of the system reads.
- **Observability** — structured stderr logging and a queryable
  connection-status interface.
- **Lifecycle** — explicit `init` / `run` / `shutdown` with no global
  constructors.
- **Provider & model registry** — reads the committed configuration
  subtree (e.g. `providers.*`) and builds the runtime provider/model table
  from it. The registry holds the *running* view; the config system owns
  the candidate/running lifecycle.

---

## Stage 4 — System diagram

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              main()                                         │
│                   init → load config → run loop → shutdown                  │
└─────────────────────────────────────────────────────────────────────────────┘
                              │
        ┌─────────────────────┴─────────────────────┐
        │                                           │
        │           UI / Client Process             │
        │                                           │
        │  ┌─────────────┐      ┌─────────────┐    │
        │  │  Event Bus  │      │  Output Bus │    │
        │  │  (IPC in)   │      │  (IPC out)  │    │
        │  └──────┬──────┘      └──────▲──────┘    │
        │         │                    │            │
        │  ┌──────┴──────┐      ┌──────┴──────┐    │
        │  │ CLI / Web / │      │  Response   │    │
        │  │  External   │      │  Renderer   │    │
        │  │    UI       │      │             │    │
        │  └─────────────┘      └─────────────┘    │
        │                                           │
        └─────────────────────┬─────────────────────┘
                              │
                              ▼
        ┌─────────────────────────────────────────────────┐
        │              IPC Fabric / Message Bus           │
        │   (UI process ↔ main core ↔ auxiliary cores)    │
        └─────────────────────────────────────────────────┘
                              │
                              │
        ┌─────────────────────┴─────────────────────┐
        │                                           │
        │           Main Core Process               │
        │                                           │
        │  ┌─────────────────────────────────────┐  │
        │  │      Process Orchestrator           │  │
        │  │  - spawns aux cores                 │  │
        │  │  - receives health/capacity reports │  │
        │  │  - decides subagent placement       │  │
        │  └────────────────┬────────────────────┘  │
        │                   │                       │
        │  ┌────────────────┼────────────────┐      │
        │  │                │                │      │
        │  ▼                ▼                ▼      │
        │ ┌─────────┐  ┌───────────┐  ┌──────────┐ │
        │ │ Agent   │  │ Handler   │  │ Provider │ │
        │ │ Core    │  │ Engine    │  │ Registry │ │
        │ │         │  │           │  │          │ │
        │ │ manages │  │           │  │          │ │
        │ │subagents│  │           │  │          │ │
        │ └────┬────┘  └───────────┘  └──────────┘ │
        │      │                                  │
        │      └────────────────┬─────────────────┘
        │                       │
        │          ┌────────────▼────────────┐     │
        │          │   Network Transport     │     │
        │          │   Config System         │     │
        │          │   Observability         │     │
        │          └─────────────────────────┘     │
        │                                           │
        └─────────────────────┬─────────────────────┘
                              │ spawns / manages
            ┌─────────────────┼─────────────────┐
            │                 │                 │
            ▼                 ▼                 ▼
   ┌─────────────────┐ ┌─────────────────┐ ┌─────────────────┐
   │ Auxiliary Core  │ │ Auxiliary Core  │ │ Auxiliary Core  │
   │   Process 1     │ │   Process 2     │ │   Process N     │
   │                 │ │                 │ │                 │
   │  ┌───────────┐  │ │  ┌───────────┐  │ │  ┌───────────┐  │
   │  │   Agent   │  │ │  │   Agent   │  │ │  │   Agent   │  │
   │  │   Core    │  │ │  │   Core    │  │ │  │   Core    │  │
   │  │ +subagents│  │ │  │ +subagents│  │ │  │ +subagents│  │
   │  └───────────┘  │ │  └───────────┘  │ │  └───────────┘  │
   │                 │ │                 │ │                 │
   │  ┌───────────┐  │ │  ┌───────────┐  │ │  ┌───────────┐  │
   │  │ Handler   │  │ │  │ Handler   │  │ │  │ Handler   │  │
   │  │ Engine    │  │ │  │ Engine    │  │ │  │ Engine    │  │
   │  ├───────────┤  │ │  ├───────────┤  │ │  ├───────────┤  │
   │  │ Provider  │  │ │  │ Provider  │  │ │  │ Provider  │  │
   │  │ Registry  │  │ │  │ Registry  │  │ │  │ Registry  │  │
   │  ├───────────┤  │ │  ├───────────┤  │ │  ├───────────┤  │
   │  │ Network   │  │ │  │ Network   │  │ │  │ Network   │  │
   │  │ Transport │  │ │  │ Transport │  │ │  │ Transport │  │
   │  ├───────────┤  │ │  ├───────────┤  │ │  ├───────────┤  │
   │  │ Config    │  │ │  │ Config    │  │ │  │ Config    │  │
   │  │ System    │  │ │  │ System    │  │ │  │ System    │  │
   │  ├───────────┤  │ │  ├───────────┤  │ │  ├───────────┤  │
   │  │Observabil.│  │ │  │Observabil.│  │ │  │Observabil.│  │
   │  └───────────┘  │ │  └───────────┘  │ │  └───────────┘  │
   └─────────────────┘ └─────────────────┘ └─────────────────┘
```

### Component responsibilities

| Component | Owns |
|-----------|------|
| **UI / Client Process** | User-facing surface: CLI, web UI, external clients. |
| **IPC Fabric** | Many-to-many message bus connecting UI, main core, and auxiliary cores. |
| **Process Orchestrator** | Spawns aux cores, tracks their health/capacity, decides subagent placement. Lives in main core only. |
| **Core Engine** | Replicable unit inside every core process: Agent Core, Handler Engine, Provider Registry, Network Transport, Config System, Observability. |
| **Agent Core** | Per-process lifecycle, session/subagent state, state machine dispatch, event queue. |
| **Handler Engine** | LLM calls, tool execution, file I/O, result validation. |
| **Provider & Endpoint Registry** | Endpoint URLs, API versions, credentials, model IDs, capabilities. |
| **Network Transport** | HTTP client, connection tracking, streaming, stall detection, retry. |
| **Config System** | Hierarchical config tree, candidate/running state, commit/rollback. |
| **Observability** | Logging, tracing, in-flight connection status. |

### Key architectural decisions

- **Main + auxiliary core processes.** A single main core process can run
  subagents itself and, when needed, spawn auxiliary core processes to
  distribute load. It can also be configured to orchestrate only.
- **Centralised placement.** The Process Orchestrator decides which core
  hosts a new subagent, using health and capacity reports from auxiliary
  cores.
- **Headless by default.** The core can run without a UI attached. A UI
  may connect, disconnect, and reconnect over the IPC Fabric without
  stopping the session.
- **No global constructors.** Every context is caller-allocated; every
  subsystem is explicitly initialised and torn down.
- **Static dispatch tables.** Provider and toolkit integration uses
  indexer-friendly static tables, not dynamic registration.

---

## Tool Execution Framework

cog-toolkit is the execution layer for tools invoked by the LLM during an
agent loop.  It sits between the core state machine and the concrete tool
implementations registered in the static @c cog_tool_table_t defined in
cog-core.

### Responsibilities

1. **Prepare the tool call context.**  A @c cog_toolkit_call_t carries the
tool name, serialised request payload, optional per-call timeout, and a
cancellation flag.  Callers create and configure the context, then pass it to
the dispatcher.

2. **Dispatch tools by name.**  @c cog_toolkit_execute resolves the tool in
the static table, invokes its @c cog_tool_fn implementation, and writes the
outcome into a caller-provided @c cog_toolkit_result_t.

3. **Run multiple tools.**  @c cog_toolkit_execute_all accepts an array of
call contexts and an options struct (@c timeout_ms, @c max_parallel,
@c stop_on_error).  It returns an opaque
@c cog_toolkit_result_collection_t that preserves call order and owns
copies of the individual results.

4. **Validate results.**  Result values expose a status code, an output
buffer, and an optional error message.  A schema validation helper checks the
output against an expected contract before the result is fed back into the
core event queue.

5. **Feed results back as events.**  On completion, the toolkit constructs a
@c COG_EVENT_TOOL_RESULT event whose payload references the validated result.
The core's event queue receives this event and resumes state-machine dispatch.

### Design constraints

- **No dynamic registration.**  Tools are looked up exclusively in the static
const @c cog_tool_table_t supplied to the core.
- **Caller-allocated state.**  Every toolkit object is created through an
explicit constructor and destroyed through a matching destructor.  The library
never allocates hidden buffers.
- **Fail closed.**  Every public function returns @c cog_result_t.  Errors are
propagated rather than swallowed.
- **Single-threaded by default.**  Each call context and result collection is
owned by the thread that created it or performed the execution.  Parallel
execution is cooperative and bounded by @c max_parallel; thread safety can
be layered on top later with external locking.

---

## Next steps

Stage 5 — Research & PoCs. The main unknowns to test:

1. IPC mechanism for the fabric (Unix domain sockets, named pipes, shared
   memory, etc.).
2. Process spawning model on Linux (`fork`/`exec`, `posix_spawn`, etc.).
3. HTTP client + streaming for LLM providers.
4. Hierarchical config commit/rollback data structure.
