# Cog — Complete User Session Walkthrough

A step-by-step trace of what happens when a user starts Cog and asks a
simple question. Every function call, every state change, every allocation.

---

## The Scenario

User starts `cog-cli` and types:

```
$ cog
> What is the capital of France?
```

The LLM responds with a streaming answer. The user sees it appear token
by token. Then the session exits cleanly.

---

## Phase 0: Process Startup (before main)

**Execution context:** Operating system loader

```
OS loads cog-cli binary
  → .text, .data, .bss mapped
  → No Cog code runs yet (no global constructors)
  → No allocations
```

**State:** Nothing. No Cog objects exist.

---

## Phase 1: main() — Application Setup

**Execution context:** `applications/cog-cli/src/main.c`

### Step 1: Create the session

```c
cog_session_t *session = NULL;
cog_result_t rc = cog_session_new(&session, NULL);
// rc == COG_OK
// session != NULL
```

**What happens inside `cog_session_new`:**
1. `malloc(sizeof(struct cog_session))` — the session struct
2. `session->id = generate_uuid()` — e.g. `"sess-7a3f-9b2c"`
3. `session->root = alloc_node(ROLE_SYSTEM)` — root node with no parent
4. `session->active = session->root`
5. `session->node_count = 1`
6. `*out = session`

**Memory allocated:** `sizeof(cog_session_t)` + root node

**State:**
```
session: id="sess-7a3f-9b2c"
  root: node_id=1, parent_id=0, role=SYSTEM, content=NULL
  active: root
```

---

### Step 2: Create the IPC fabric (multi-process)

```c
cog_ipc_fabric_t *fabric = NULL;
rc = cog_ipc_fabric_new(&fabric, NULL);
```

**What happens:**
1. `malloc(sizeof(struct cog_ipc_fabric))`
2. `fabric->socket_path = "/tmp/cog-<uid>.sock"`
3. `fabric->peers = alloc_table(16)` — peer routing table
4. `fabric->next_peer_id = 1`

**State:**
```
fabric: socket="/tmp/cog-1000.sock"
  peers: empty (capacity 16)
```

---

### Step 3: Create the IPC endpoint

```c
cog_ipc_endpoint_t *endpoint = NULL;
cog_ipc_endpoint_config_t ipc_cfg = {
    .fabric = fabric,
    .session_id = "sess-7a3f-9b2c",
};
rc = cog_ipc_endpoint_new(&endpoint, &ipc_cfg);
```

**What happens:**
1. `malloc(sizeof(struct cog_ipc_endpoint))`
2. `endpoint->fabric = fabric` (borrowed)
3. `endpoint->session_id = strdup("sess-7a3f-9b2c")` (owned copy)
4. `endpoint->rx_queue = alloc_queue(256)` — incoming message buffer
5. `endpoint->tx_queue = alloc_queue(256)` — outgoing message buffer
6. `cog_ipc_endpoint_register(endpoint)` — adds to fabric's peer table

**State:**
```
fabric:
  peers[0]: peer_id=1, session="sess-7a3f-9b2c", endpoint=<endpoint>
```

---

### Step 4: Create the observability context

```c
cog_obs_t *obs = NULL;
cog_obs_config_t obs_cfg = {
    .min_severity = COG_OBS_INFO,
    .service = "cog-cli",
    .conn_event_sink = my_gui_sink,  // for live connection table
};
rc = cog_obs_new(&obs, &obs_cfg);
```

**What happens:**
1. `malloc(sizeof(struct cog_obs))`
2. `obs->span_stack = alloc_stack(64)`
3. `obs->diag_table = alloc_table(32)`
4. `obs->scratch_buf = malloc(4096)` — formatting buffer
5. `obs->conn_event_sink = my_gui_sink`

**State:**
```
obs: service="cog-cli"
  span_stack: empty
  diag_table: empty (capacity 32)
```

---

### Step 5: Create the provider

```c
cog_provider_t *provider = NULL;
cog_provider_config_t prov_cfg = {
    .name = "openai",
    .endpoint = "https://api.openai.com/v1",
    .api_key = getenv("OPENAI_API_KEY"),
};
rc = cog_provider_connect(&provider, &prov_cfg);
```

**What happens:**
1. `malloc(sizeof(struct cog_provider))`
2. `provider->name = strdup("openai")`
3. `provider->endpoint = strdup("https://api.openai.com/v1")`
4. `provider->vtable = &openai_vtable` — static const dispatch table
5. `provider->http = cog_net_new(...)` — internal HTTP client

**State:**
```
provider: name="openai", endpoint="api.openai.com"
  vtable: &openai_vtable
  http: <cog_net_t>
```

---

### Step 6: Create the core

```c
cog_core_t *core = NULL;
cog_core_config_t core_cfg = {
    .session = session,
    .dispatch_table = my_dispatch_table,
    .dispatch_table_size = 12,
    .provider = provider,
    .obs = obs,
    .ipc_endpoint = endpoint,
    .local_peer_id = 1,
};
rc = cog_core_new(&core, &core_cfg);
```

**What happens:**
1. `malloc(sizeof(struct cog_core))`
2. `core->session = session` (borrowed)
3. `core->provider = provider` (borrowed)
4. `core->obs = obs` (borrowed)
5. `core->ipc = endpoint` (owned — core will free it)
6. `core->event_queue = alloc_queue(256)`
7. `core->state = STATE_IDLE`
8. `core->pending_queue = alloc_queue(8)` — for queued messages

**State:**
```
core:
  session: "sess-7a3f-9b2c"
  state: IDLE
  event_queue: empty (capacity 256)
  pending_queue: empty (capacity 8)
```

---

## Phase 2: User Input

**Execution context:** UI thread (or main thread if single-process)

### Step 7: User types and hits enter

```
User types: "What is the capital of France?"
User presses: Enter
```

**In the UI process:**

```c
// UI captures input
const char *input = "What is the capital of France?";
size_t len = strlen(input);

// Create event
cog_event_t event = {
    .id = 1,
    .kind = COG_EVENT_USER_INPUT,
    .payload = input,
    .payload_len = len,
    .session_id = "sess-7a3f-9b2c",
};

// Send to main core via IPC
cog_ipc_endpoint_send(endpoint, &event);
```

**What happens in `cog_ipc_endpoint_send`:**
1. Serialize event to wire format (msgpack or JSON)
2. `write(endpoint->tx_fd, serialized, len)`
3. Return COG_OK

---

### Step 8: Main core receives the event

**Execution context:** Main core thread running `cog_core_run`

```c
// Inside cog_core_run's poll loop
cog_ipc_endpoint_poll(endpoint);
```

**What happens:**
1. `read(endpoint->rx_fd, buf, sizeof(buf))` — reads serialized event
2. Deserialize into `cog_event_t`
3. `cog_core_enqueue(core, &event)`

**Inside `cog_core_enqueue`:**
1. `queue_push(core->event_queue, &event)`
2. Return COG_OK

**State:**
```
core->event_queue: [USER_INPUT("What is the capital of France?")]
```

---

## Phase 3: Event Dispatch

**Execution context:** Main core thread, inside `cog_core_run`

### Step 9: Core dequeues the event

```c
// Inside cog_core_run
cog_event_t event;
rc = cog_event_queue_pop(core->event_queue, &event);
// event.kind == COG_EVENT_USER_INPUT
```

### Step 10: State machine lookup

```c
// Current state: IDLE
// Event: USER_INPUT
cog_dispatch_entry_t *entry = cog_state_lookup(
    core->dispatch_table,
    core->dispatch_table_size,
    core->state,        // IDLE
    event.kind          // USER_INPUT
);
```

**Dispatch table entry:**
```c
{
    .from_state = STATE_IDLE,
    .event_kind = COG_EVENT_USER_INPUT,
    .to_state = STATE_AWAITING_LLM,
    .handler = handle_user_input,
}
```

### Step 11: Handler executes

**Execution context:** `handle_user_input` in application code

```c
cog_result_t handle_user_input(cog_core_t *core, const cog_event_t *event) {
    // 1. Add user message to session
    cog_message_t msg = {
        .role = COG_MESSAGE_ROLE_USER,
        .content = event->payload,
        .content_len = event->payload_len,
        .name = NULL,
    };
    cog_session_node_t *node = NULL;
    cog_session_add_message(core->session, NULL, &msg, &node);
    // node: id=2, parent=root, role=USER, content="What is the capital of France?"
    
    // 2. Get conversation history
    const cog_message_t *history = NULL;
    size_t history_count = 0;
    cog_session_get_history(core->session, node, &history, &history_count);
    // history: [msg] (just the user message, no prior context)
    
    // 3. Send to provider
    cog_provider_request_t req = {
        .messages = history,
        .message_count = history_count,
        .model = "gpt-4",
        .stream = true,
    };
    cog_provider_send(core->provider, &req);
    
    // 4. Return next state
    return STATE_AWAITING_LLM;
}
```

**State:**
```
session:
  root: id=1, role=SYSTEM
    └─ node2: id=2, parent=1, role=USER, content="What is the capital of France?"
  active: node2

provider:
  in_flight: true
  request_id: 42

core->state: AWAITING_LLM
```

---

## Phase 4: Streaming Response

**Execution context:** Main core thread, event loop polling

### Step 12: First chunk arrives

```c
// Inside cog_core_run poll loop
cog_net_poll(provider->http);
```

**What happens:**
1. `recv(http->fd, buf, sizeof(buf))` — reads HTTP chunk
2. Parse SSE (Server-Sent Events): `data: {"choices":[{"delta":{"content":"Paris"}}]}`
3. Extract content: `"Paris"`
4. Create event: `COG_EVENT_LLM_CHUNK`
5. `cog_core_enqueue(core, &chunk_event)`

**State:**
```
core->event_queue: [LLM_CHUNK("Paris")]
```

### Step 13: Chunk handler

```c
cog_result_t handle_llm_chunk(cog_core_t *core, const cog_event_t *event) {
    // 1. Append to current assistant node
    cog_session_append_chunk(core->session, core->session->active, 
                              event->payload, event->payload_len);
    
    // 2. Forward to UI
    cog_ipc_endpoint_send(core->ipc, event);
    
    // 3. Stay in same state
    return STATE_AWAITING_LLM;
}
```

**State:**
```
session:
  node3: id=3, parent=2, role=ASSISTANT, content="Paris" (partial)
  active: node3

provider:
  bytes_in: 128
  bytes_out: 256
```

### Step 14: More chunks

```
Chunk 2: " is"
Chunk 3: " the"
Chunk 4: " capital"
...
Chunk N: "."
```

Each chunk appends to node3's content and forwards to UI.

**State after all chunks:**
```
session:
  node3: content="Paris is the capital of France."
```

---

## Phase 5: Completion

### Step 15: LLM complete event

```c
// Provider signals end of stream
COG_EVENT_LLM_COMPLETE
```

**Handler:**
```c
cog_result_t handle_llm_complete(cog_core_t *core, const cog_event_t *event) {
    // 1. Finalize assistant node
    cog_session_finalize_node(core->session, core->session->active);
    
    // 2. Set timing
    cog_timing_t timing = {
        .request_sent_ns = ...,
        .first_token_ns = ...,
        .last_token_ns = ...,
        .total_tokens = 8,
    };
    cog_session_set_timing(core->session, core->session->active, &timing);
    
    // 3. Set cost
    cog_cost_t cost = {
        .input_tokens = 12,
        .output_tokens = 8,
        .total_cost = 0.0004,
    };
    cog_session_set_cost(core->session, core->session->active, &cost);
    
    // 4. Auto-save
    cog_store_save(core->store, core->session);
    
    // 5. Return to idle
    return STATE_IDLE;
}
```

**State:**
```
session:
  root: id=1
    └─ node2: id=2, role=USER, content="What is the capital of France?"
        └─ node3: id=3, role=ASSISTANT, content="Paris is the capital of France."
           timing: {ttft=150ms, total=800ms}
           cost: {in=12, out=8, $0.0004}
  active: node3

core->state: IDLE
```

---

## Phase 6: UI Update

**Execution context:** UI process

```c
// UI receives the complete event
COG_EVENT_LLM_COMPLETE

// UI re-enables input
printf("\n> ");
fflush(stdout);
```

---

## Phase 7: Shutdown

**Execution context:** User types `exit` or hits Ctrl+D

```c
// UI sends shutdown
COG_EVENT_SHUTDOWN

// Main core handler
cog_result_t handle_shutdown(cog_core_t *core, const cog_event_t *event) {
    // 1. Save session
    cog_store_save(core->store, core->session);
    
    // 2. Cancel any in-flight
    if (core->state == STATE_AWAITING_LLM) {
        cog_provider_cancel(core->provider);
    }
    
    // 3. Drain pending queue
    // (none in this case)
    
    // 4. Return shutdown state
    return STATE_SHUTDOWN;
}
```

**Cleanup:**
```c
cog_core_free(core);        // frees core, event_queue, pending_queue, ipc_endpoint
cog_session_free(session);  // frees all nodes recursively
cog_provider_free(provider); // frees provider, http client
cog_obs_free(obs);          // frees obs, span_stack, diag_table
cog_ipc_fabric_free(fabric); // frees fabric, peer table
```

**Final state:** All memory freed. Process exits.

---

## Memory Allocation Summary

| Phase | Allocation | Size | Freed by |
|-------|-----------|------|----------|
| Session create | `cog_session_t` | ~128B | `cog_session_free` |
| Session create | Root node | ~256B | `cog_session_free` |
| IPC fabric | `cog_ipc_fabric_t` | ~256B | `cog_ipc_fabric_free` |
| IPC endpoint | `cog_ipc_endpoint_t` | ~512B | `cog_core_free` |
| Obs context | `cog_obs_t` | ~4KB | `cog_obs_free` |
| Provider | `cog_provider_t` | ~512B | `cog_provider_free` |
| Core | `cog_core_t` | ~1KB | `cog_core_free` |
| Event queue | 256 events | ~16KB | `cog_core_free` |
| Pending queue | 8 messages | ~2KB | `cog_core_free` |
| User node | node + content | ~300B | `cog_session_free` |
| Assistant node | node + content | ~350B | `cog_session_free` |

**Total peak:** ~25KB for a simple session

---

## Event Flow Diagram

```
USER_INPUT
    │
    ▼
┌─────────────┐
│    IDLE     │
└──────┬──────┘
       │ handle_user_input
       │ → add user msg
       │ → cog_provider_send()
       ▼
┌─────────────┐
│ AWAITING_LLM│◄────────┐
└──────┬──────┘         │
       │                │
       │ LLM_CHUNK      │ LLM_CHUNK
       │ → append chunk │ (repeat)
       │ → forward to UI│
       ▼                │
┌─────────────┐         │
│ AWAITING_LLM│─────────┘
└──────┬──────┘
       │ LLM_COMPLETE
       │ → finalize node
       │ → set timing/cost
       │ → auto-save
       ▼
┌─────────────┐
│    IDLE     │
└─────────────┘
```

---

## Threading Model

| Thread | What it does | Functions called |
|--------|-------------|------------------|
| Main (core) | Event loop | `cog_core_run`, `cog_core_enqueue`, handlers |
| UI | Input/output | `cog_ipc_endpoint_send`, `cog_ipc_endpoint_poll` |
| Provider | HTTP I/O | `cog_net_poll`, `cog_net_send` |

**Synchronization:** Single-threaded core. UI and provider communicate
via IPC events. No shared mutable state between threads.

---

## Error Paths

| Error | Where | Handling |
|-------|-------|----------|
| `COG_ERROR_NOMEM` | Any allocation | Log fatal, exit |
| `COG_ERROR_NETWORK` | Provider send | Retry with backoff |
| `COG_ERROR_ABORTED` | User cancel | Discard partial, return to IDLE |
| `COG_ERROR_TIMEOUT` | Provider stall | Retry or fail |
| `COG_ERROR_INVALID_ARG` | Bad input | Log error, return to IDLE |

---

## This Walkthrough Shows

1. **Every allocation is visible** — no hidden malloc
2. **Every event is explicit** — no hidden state transitions
3. **Every function has a contract** — documented in headers
4. **Session tree is consistent** — no orphaned nodes
5. **Cleanup is complete** — every allocation freed
6. **Threading is simple** — single-threaded core, IPC for UI
