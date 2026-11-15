# Cog — System Design Blueprint

> Stage 14.H provider delivery passed independent revision review and is
> integrated. [H evidence](REAL_CLIENT_H_EVIDENCE.md) records bounded JSON/SSE,
> retained typed output, final opaque preservation, mock conformance, offline
> TLS and installed link closure. K conversation-loop migration and full release
> validation remain pending.

> Stage 14.Q runtime implementation passed independent revision review and is integrated;
> [Q evidence](REAL_CLIENT_Q_EVIDENCE.md) records bounded nonblocking IPC,
> fixed core reservations and fair rotating local/IPC dispatch. External event
> capacity stays configured; separate IPC ingress descriptors derive from the
> configured IPC service quantum to prevent saturation starvation. Combined
> real G-library validation remains tracked before full release gate approval.


> [Stage 14.E final review](REAL_CLIENT_PASS3.md) passed after the additional
> independent review of `8563f81`. The three header passes and final review are
> complete; implementation and executable conformance remain delivery work.

> Current extension: [real-client blueprint](REAL_CLIENT_BLUEPRINT.md) and
> [Stage 14 execution scaffold](REAL_CLIENT_WBS.md). The material below
> describes the original engine/mock-CLI baseline, not real-client release
> completion. Stage 14 uses technical gates under delegated design authority.
> [Transport research](research/https-lifecycle.md) is accepted as conditional
> evidence for the accepted header profile; runtime lifecycle and maintained-build
> conformance is recorded by [transport delivery F](REAL_CLIENT_F_EVIDENCE.md).
> The initial no-replay profile uses fresh HTTP/1.1 connections, no pooling or
> HTTP/2, and a second-attempt guard. Full release validation remains pending.
> DNS follow-up requires a c-ares primary resolver with
> threaded resolution disabled, empirical shipped-profile validation and explicit
> rejection of unsupported builds. Threaded/QUICK_EXIT alternatives fail cleanup.
> Core fairness delivery Q must precede the real conversation-loop delivery K.
> [Protocol research](research/provider-wire.md) selects private yyjson with
> explicit pools and records two synthetic live requests; opaque continuation
> and recoverable delta-capacity contracts now require implementation conformance.

> [Stage 14.C public contracts](REAL_CLIENT_CONTRACTS.md) now define initial
> amendments to the baseline below: public provider base plus typed nonblocking
> operations, structured messages/continuation, v2 durable tool intent, async
> policy-gated tools, explicit c-ares transport ownership and fair core service.
> These amendments supersede corresponding baseline four-operation provider,
> synchronous toolkit, unversioned-message and automatic transport retry text.
> Cog workspaces are constructor-bounded; third-party handle runtime allocation
> is explicitly declared and total dependency-memory caps are unsupported. Final E
> review accepts that declared profile. Transport F is integrated; sibling
> implementation and full release gates remain pending.

The [Stage 14.D pass-2 scenario register](REAL_CLIENT_SCENARIO_PASS2.md) records
the current header amendments, all original20 plus R01–R18 traces and delivery
ownership. Quiescent same-host guarded checkpoint migration is the supported
real-client profile; historical cross-core sketches below do not imply live
handle transfer or automatic replay of effects. Independent gate E passed.

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
- **Session state** — hierarchical conversation tree with forking, timing,
  and billing. Each node is a message exchange; branches share immutable
  ancestors. Persistence via pluggable backend (file, SQLite, custom).
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

## Bounded configuration delivery

Stage 14.J follows the accepted header contract: constructor-reserved aggregate
node/string/transaction/validator pools, no allocation during tree operations,
and immutable caller-owned endpoint generations retained through disconnect.
[Config contract](../libraries/cog-config/contracts/CONTRACT.md) records contiguous
string admission and tombstone ceilings. [CLI configuration](../applications/cog-cli/CONFIGURATION.md)
defines the bounded dotted-key file grammar, exact presence/precedence, required
explicit REAL model, durable XDG/HOME default, text-only no-store policy and
construction-only credential resolution/clearing. No implicit mock fallback or
runtime dependency on pi/pass is introduced. H/K still own provider dispatch,
durable guard admission and complete application-loop migration.

## Bounded agent lifecycle

Core-local delivery passed independent review and is integrated;
[local-agent integration evidence](REAL_CLIENT_LOCAL_AGENT_INTEGRATION.md)
records the focused checks and remaining K/N/O application/runtime gaps.

The core reserves agent records and copied strings at construction. Spawn and
cancel admit leased intent events atomically to the core queue; application
dispatch owns execution or forwarding. The handler copies configuration and
intent before deferring work, and release cannot reclaim a queued or dispatching
intent. A current owner/epoch/sequence update retains one notification until
explicit copy and acknowledgement; terminal results remain readable until
release. Process placement, transport authentication, and remote effects stay
in the application and orchestrator integration (Stage 14.O).

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

The Stage 14 approved contracts in [REAL_CLIENT_CONTRACTS.md](REAL_CLIENT_CONTRACTS.md)
and `cog/tool.h` / toolkit public headers define the current execution profile.
The toolkit resolves only static descriptors. It validates complete strict JSON
and its bounded schema subset before constructing an execution; unknown validation
keywords fail UNSUPPORTED. Codec state uses private constructor-reserved pools.

The responsive path uses new/start/poll/cancel/result/destroy callbacks. Explicit
ALLOW follows application policy and a successfully persisted STARTED ledger.
UNDECIDED cannot execute; DENY is terminal without effects. Constructor and start
check ownership/unsafe session state, and the execution holds its guard borrow
through cleanup. Poll/cancel do bounded work without sleeping; free refuses active
children. Retained output can be copied repeatedly after overflow without replay.

Workspace read/search/edit uses an opened root, component-wise no-follow directory
traversal and descriptor-relative I/O. Commands are separately enabled, use copied
explicit environment, and own a process group supervised by a helper. Subreaper
and signal changes are confined to the helper. TERM/KILL cancellation drains and
reaps inherited-group descendants before completion. This grants process execution,
not OS confinement; group escape is outside the supported cancellation profile.

Synchronous compatibility calls own copied input and a fixed 64 KiB capture. A
context invokes its handler at most once, even after overflow. Batch execution
reserves every result/worker slot before effects and defaults to sequential;
parallelism is explicit and requires REENTRANT|INDEPENDENT descriptors. Worker
startup is barrier-gated so partial creation failure executes nothing.
`stop_on_error` retains every launched outcome and marks unstarted calls ABORTED.
Blocking compatibility calls refuse fenced owner stores and are excluded from
the real-client service loop.

Implementation limits, output formats, dependency provenance and test mapping
are in [the toolkit contract](../libraries/cog-toolkit/contracts/CONTRACT.md).

---

## IPC Fabric Transport Layer (cog-ipc)

cog-ipc is the message transport layer that connects UI processes, the main
core, and auxiliary cores over Unix domain sockets.  It is implemented by the
@c cog-ipc library and follows the same design rules as the rest of Cog:
caller-allocated contexts, no hidden allocations, static const tables where
applicable, and every public function returns @c cog_result_t.

### Public types

- @c cog_ipc_endpoint_t — opaque endpoint wrapping a Unix domain socket.  An
  endpoint may be a listener, an outgoing connection, or an accepted
  connection.
- @c cog_peer_id_t — opaque numeric identifier assigned to a connected peer
  by @c cog_ipc_endpoint_accept or @c cog_ipc_endpoint_connect.  The value
  @c COG_PEER_ID_INVALID denotes no peer.
- @c cog_ipc_message_t — caller-owned message buffer carrying a payload and
  its length/capacity.  The buffer is allocated with
  @c cog_ipc_message_new and released with @c cog_ipc_message_free.

### Lifecycle

Endpoints are created with @c cog_ipc_endpoint_new and destroyed with
@c cog_ipc_endpoint_free.  A single endpoint can be reused after
@c cog_ipc_endpoint_disconnect returns it to the unbound state.  Messages are
independent of endpoints: the caller supplies the payload buffer and the
library only frames and transmits it.

### Operations

- @c cog_ipc_endpoint_listen binds and listens on a filesystem path.
- @c cog_ipc_endpoint_accept creates a new connected endpoint from a
  pending listener connection.
- @c cog_ipc_endpoint_connect connects to a listening path.
- @c cog_ipc_endpoint_send and @c cog_ipc_endpoint_recv exchange framed
  messages on a connected endpoint.
- @c cog_ipc_endpoint_poll waits for readability, writability, or errors.
- @c cog_ipc_endpoint_disconnect closes the socket and returns the endpoint
  to the unbound state.

### Design constraints

- **Connection-oriented transport.**  The header contract assumes reliable,
  ordered, stream-oriented Unix domain sockets.  Message framing is the
  responsibility of the implementation.
- **No dynamic registration.**  Peer and endpoint tables are not exposed;
  the API is intentionally minimal.
- **Caller-allocated state.**  Every endpoint and every message buffer is
  explicitly created and destroyed by the caller.
- **Fail closed.**  Every public function returns @c cog_result_t.
- **Single-threaded per endpoint.**  Each endpoint is owned by one thread at
  a time.  Distinct endpoints may be used concurrently without locking.
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

## Session Tree

The session is a tree of conversation nodes, not a flat list. Each node
represents a single message exchange with full context:

- **Role**: system, user, assistant, tool_call, tool_result
- **Content**: message text, tool arguments, tool results
- **Raw data**: provider request/response JSON, chunk count
- **Timing**: request_sent, first_token, last_token, duration, ttft, tokens/sec
- **Billing**: input/output tokens, cost per token, total cost
- **Identity**: node_id, parent_id for persistence

### Forking

`cog_session_fork()` creates a new branch from any node. The forked branch
shares all ancestor nodes (immutable) but has its own children. This makes
parallel exploration, subagent spawning, and fan-out/fan-in patterns trivial.

### History building

`cog_session_get_history()` walks from root to a given node and returns the
message array for LLM context. The active path defines the current
conversation; other branches are preserved but not sent.

### Cumulative metrics

`cog_session_cumulative_cost()` and `cog_session_cumulative_timing()` sum
values up the tree, giving total cost and latency for any conversation path.

---

## Session Persistence

Sessions are persisted via a pluggable backend (`cog_store_vtable_t`):

- **File backend** (default): JSONL node log + binary index per session
- **Null backend**: no-op for testing
- **Custom backends**: implement the vtable for SQLite, RocksDB, etc.

The store supports incremental saves (`append`), full saves, loads, deletes,
and listing. Auto-save after each exchange is configurable.

---

## Network Transport

cog-net provides HTTP client functionality per process:

- **Request lifecycle**: request IDs, cancellation, active count
- **Streaming**: SSE/chunked response handling
- **Stall detection**: `COG_EVENT_NETWORK_STALL` after `stall_timeout_ms`
- **Retry**: automatic with configurable max_retries and backoff
- **Recovery**: `COG_EVENT_NETWORK_RECOVERED` on reconnect
- **Rate limiting**: `cog_provider_error_t` with retryable flag and retry_after_ms

---

## Orchestrator

The Process Orchestrator manages auxiliary core lifecycle:

- **Spawn**: create new aux core processes
- **Health**: monitor via health reports
- **Placement**: decide which core hosts a subagent
- **Shutdown**: graceful stop with timeout
- **Recovery**: `cog_orchestrator_sessions()` lists sessions on a crashed core
- **Auto-replace**: optional automatic replacement of dead cores

---

## Next steps

Stage 6 — Headers first. Design all public APIs as headers before any
implementation. Three iterations:
1. Initial headers from this design
2. Scenario traces to find gaps
3. Full review for consistency and edge cases

Stage 7 — Implementation. See `docs/IMPLEMENTATION_STRATEGY.md`.

## Stage 14.G shared fork points

The structured session implementation preserves assistant/effect identity by
returning the existing immutable shared fork point. A nonleaf selection retains
the current active leaf until explicit `add_message(parent=fork_point)` creates
and selects a divergent child. A leaf selection may select that leaf. This
keeps immediate snapshots valid without copying effect-bearing nodes.

### Stage 14.H provider delivery boundary

The H implementation follows the approved typed provider contract: constructor
pools, strict private namespaced yyjson, request serialization before send,
retained transactional deltas, private fragmented tool/opaque assembly and
terminal validation before tool readiness. A single ordinary retained slot plus
a resumable final cursor and independent FINISH satisfy bounded expansion even
with an entry ceiling of one. Actual net composition uses request-local peek/ack
and never retries. The [mock fixture amendment](REAL_CLIENT_H_MOCK_AMENDMENT.md)
adds a header-first typed script constructor for K's subsequent tool-loop tests.
H review/integration evidence does not waive the remaining application gates.

### Stage 14.K application composition

The single-process CLI now composes typed provider poll/next/cancel/error with
fair core service, one complete assistant node per wire turn, immutable J
configuration generations, and sequential asynchronous built-in workspace tools.
The application owns the sole finite retry budget and saves STARTED before a
tool starts, then result+terminal ledger before model continuation. Any uncertain
save freezes admission; a terminal tool result whose insertion/save fails stays
retained and visible without another execution. Earlier queued prompts cannot
authorize continuation of an incomplete/cancelled turn: fresh postfailure input
forks from a safe ancestor and preserves the failed branch. CGD1 visible snapshots
include active/incomplete state without opaque continuation. Main explicitly
owns dependency initialization and signalfd lifecycle; no global signal target
is introduced. [Application contract](../applications/cog-cli/CONTRACT.md) and
[K evidence](REAL_CLIENT_K_EVIDENCE.md) distinguish this single-process delivery
from L recovery interaction and N/O remote/headless orchestration.
