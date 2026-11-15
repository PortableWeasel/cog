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
- **Observability** — structured stderr logging through a static sink
  table, an explicit span stack for tracing, and a fixed-capacity,
  queryable table of connection snapshots.
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

## Provider & Endpoint Registry

The Provider & Endpoint Registry is the boundary between the agent core
and external LLM services.  It is implemented by the `cog-provider`
library and follows the same static-table, caller-allocated design as
`cog-core`.

### Endpoint table

An endpoint is a concrete provider deployment: a base URL, an API
version, a credential, a pointer to a provider implementation vtable,
and a pointer to a model table.  Endpoints are stored in a static const
array (`cog_endpoint_table_t`) and looked up by name.  There is no
dynamic registration; the caller links the desired provider
implementations and supplies the table at startup.

### Provider vtable

Each provider implementation exposes a static vtable with four required
operations:

- **connect** — create and initialise a provider instance for an
  endpoint.
- **send** — submit an LLM request.
- **receive** — read the next response or streaming chunk.
- **disconnect** — tear down the provider instance.

Both single-turn and streaming calls use the same operations.  A
streaming request sets `cog_provider_request_t.stream` to true; the
caller then loops on `cog_provider_receive` until
`cog_provider_response_t.is_complete` is true.

### Model table

Each endpoint references a static table of models (`cog_model_table_t`).
A model descriptor carries the endpoint-specific model id, a display
name, the context window size, a bitmask of capability flags
(streaming, tools, vision), and optional per-token cost metadata.
Model lookup is by id within an endpoint; `cog_endpoint_table_select_by_model`
resolves a model id to the first endpoint that serves it.

### Lifecycle and ownership

- Endpoint and model tables are read-only and may be shared by any
  number of cores and threads.
- A `cog_provider_t` instance is created by `connect` and destroyed by
  `disconnect`; the implementation performs the allocation and matching
  free.
- Request and response buffers are caller-allocated; the provider never
  allocates hidden buffers.
- Provider instances are not thread-safe; a single instance must be used
  by only one thread at a time.

### Relationship to configuration

The registry holds the *running* view of providers.  The configuration
system owns the candidate/running lifecycle: a commit of the
`providers.*` subtree produces the endpoint table that the registry
uses.  When configuration changes, the running table is replaced
atomically after validation; in-flight provider instances are not
affected.
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

## Observability

cog-obs is the observability layer shared by a core process and any UI
attached to it.  It covers three capabilities that all draw from one
caller-allocated context, `cog_obs_t`: structured logging, trace spans, and
connection diagnostics.

### Why one context, not three subsystems

The three capabilities share a clock, a service name, and a lifetime.  A
log record wants a span id; a stall report wants to be a log record; a
health check wants both.  Giving them one context keeps that shared state
explicit instead of scattering three partially-initialised globals — and it
keeps the "no global constructors" rule intact, since `cog_obs_new` is the
only way to obtain one.

### Structured logging

Field *names* never appear as literals at call sites.  The caller declares
a static const `cog_obs_field_desc_t` per attribute, collects them into a
static const `cog_obs_field_table_t`, and references them by index:

```c
static const cog_obs_field_desc_t fields[] = {
    { "conn_id",  COG_OBS_FIELD_UINT   },
    { "endpoint", COG_OBS_FIELD_STRING },
    { "retries",  COG_OBS_FIELD_UINT   },
};
static const cog_obs_field_table_t field_table = { fields, 3 };
```

Consequences worth stating plainly:

- The emitted key set is enumerable at build time, so log consumers can be
  generated rather than discovered.
- No per-call string formatting, no key interning, no hidden allocation.
- Kind mismatches are caught at the call boundary: a field whose value kind
  disagrees with its descriptor is rejected with
  `COG_ERROR_INVALID_ARG` rather than being silently mis-rendered.

Records are formatted once into a context-owned scratch buffer as
`timestamp severity service message key=value ...` and then offered to each
sink whose own threshold admits them.  Sinks are a static const table:
there is no runtime registration, matching the provider and toolkit design.
The caller owns every destination; cog-obs never opens, closes, or flushes
anything.

`cog_obs_log_to` additionally copies the formatted text into a
caller-provided buffer, so the CLI can display exactly the line it logged.
When the buffer is too small it returns `COG_ERROR_OVERFLOW` and reports the
required capacity, allowing a retry with a larger buffer.

### Trace spans

`cog_obs_trace_begin` pushes a span onto an explicit stack owned by the
context; `cog_obs_trace_end` pops it, computes a duration, and hands a
`cog_obs_span_record_t` to the configured trace sink.  The parent of a new
span is whatever is currently on top of the stack.

The deliberate choice here is **no thread-local ambient span**.  An ambient
span would make `begin`/`end` convenient but would also mean the concept of
"the current span" lives outside any struct — precisely the hidden global
state rule 2 forbids.  Instead the tree shape is derived from the order of
`begin`/`end` calls, and spans must be closed in reverse order; closing out
of order returns `COG_ERROR_INVALID_ARG`.  The stack has a fixed capacity
taken from the configuration and returns `COG_ERROR_OVERFLOW` rather than
growing.

A NULL trace sink is legal and costs one branch: spans are still tracked so
that `cog_obs_trace_current` and `cog_obs_trace_depth` work for callers that
want structural context without emitting anything.

### Connection diagnostics

The transport owns the truth about its connections.  cog-obs provides a
fixed-capacity table of snapshots that the transport publishes into, so that
the CLI, a UI process, or a health report can query in-flight state without
touching transport internals:

- `cog_obs_diag_publish` inserts or updates a snapshot keyed by
  `conn_id`, taking the first free slot for new ids.
- `cog_obs_diag_clear` frees a slot.
- `cog_obs_diag_count` / `cog_obs_diag_at` enumerate occupied slots in slot
  order, skipping gaps.
- `cog_obs_diag_count_state` answers "how many are stalled?" directly,
  which is the question the orchestrator's health reporting actually asks.
- `cog_obs_diag_format` renders one snapshot as the same greppable text line
  the CLI prints.

A full table is an error (`COG_ERROR_OVERFLOW`), never a silent drop: losing
diagnostics is a failure mode the caller should be told about.  Snapshot
string pointers are borrowed, not copied — passing static literals is the
intended usage — so the table stays allocation-free.

### Threading and lifecycle

A `cog_obs_t` is **not** thread-safe.  It owns a mutable span stack, a
scratch buffer, and a diagnostic table, none of which are locked.  This
matches the rest of Cog: one context per thread or per core process, driven
by the thread that runs `cog_core_run`.  Field tables, sink tables, message
ids, and span names are read-only after construction and may be shared by
any number of threads.  Multi-threaded logging is achieved by creating one
context per thread or by external locking; cog-obs does not provide it.

Ownership is uniform across the library: the caller owns the context, the
strings it passes, the buffers it supplies, and every sink destination.
cog-obs owns only its own internal tables, released by `cog_obs_free`, which
discards open spans rather than emitting them.

---

## Next steps

Stage 5 — Research & PoCs. The main unknowns to test:

1. IPC mechanism for the fabric (Unix domain sockets, named pipes, shared
   memory, etc.).
2. Process spawning model on Linux (`fork`/`exec`, `posix_spawn`, etc.).
3. HTTP client + streaming for LLM providers.
4. Hierarchical config commit/rollback data structure.
