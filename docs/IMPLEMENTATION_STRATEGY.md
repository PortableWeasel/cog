# Cog — Implementation Strategy

How we implement 24 headers across 8 libraries without breaking contracts,
while parallelizing where possible.

---

## The Core Problem

Headers are contracts. Implementation must satisfy them exactly. But we have
8 libraries with interdependencies, and we want to implement them in parallel
without discovering contract violations late.

**The risk**: Two modules implemented in parallel diverge from their headers.
Integration fails. Rework.

**The mitigation**: Contract tests at module boundaries, sanity checks at
every commit, and a strict implementation order for foundational modules.

---

## Implementation Order

Modules are implemented in dependency order. Each module must pass its
sanity checks before the next module starts.

```
Phase 0: Foundation (single module, no parallelism)
  cog-core/result.h     → error codes, result type
  cog-core/message.h    → message struct (moved from cog-provider)

Phase 1: Core primitives (parallel where possible)
  cog-core/session.h    → conversation tree (depends on result, message)
  cog-core/event.h      → event kinds (depends on result)
  cog-core/state.h      → state machine (depends on event, handler)
  cog-core/handler.h    → handler types (depends on event, result)

Phase 2: Core engine (sequential — everything depends on this)
  cog-core/core.h       → agent loop (depends on all above + ipc, obs, provider)
  cog-core/agent.h      → subagent lifecycle (depends on session, result)
  cog-core/tool.h       → tool table (depends on event, result)
  cog-core/session_store.h → persistence (depends on session, result)

Phase 3: Libraries (parallel — they only depend on core headers)
  cog-ipc/              → Unix sockets, session routing
  cog-obs/              → logging, tracing, diagnostics
  cog-config/           → config tree
  cog-net/              → HTTP client, streaming

Phase 4: Provider & Toolkit (parallel)
  cog-provider/         → LLM provider abstraction
  cog-toolkit/          → tool execution framework

Phase 5: Orchestrator (depends on ipc, core)
  cog-orchestrator/     → process lifecycle

Phase 6: Application
  applications/cog-cli/ → command-line interface
```

---

## Sanity Checks

Every module has three levels of checks:

### Level 1: Header self-containment (already done)

```bash
# Every header compiles standalone with strict warnings
gcc -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Werror \
    -c -x c libraries/cog-core/include/cog/session.h -o /dev/null \
    -I libraries/cog-core/include -I libraries/cog-provider/include ...
```

**When**: On every commit that touches a header.
**Who**: CI / pre-commit hook.

### Level 2: Contract tests (per module)

Each module gets a `test_<module>_contract.c` that:

1. **Creates and destroys** every opaque type
2. **Calls every public function** with valid arguments
3. **Verifies error codes** for NULL, overflow, invalid state
4. **Checks buffer capacity** handling
5. **Validates Doxygen examples** actually compile and run

Example for `session.h`:

```c
// test_session_contract.c
#include <cog/session.h>
#include <assert.h>

int main(void) {
    // Lifecycle
    cog_session_t *s = NULL;
    assert(cog_session_new(&s, NULL) == COG_OK);
    assert(s != NULL);
    assert(cog_session_is_empty(s));
    
    // Add message
    cog_message_t msg = { COG_MESSAGE_ROLE_USER, "hello", 5, NULL };
    cog_session_node_t *node = NULL;
    assert(cog_session_add_message(s, NULL, &msg, &node) == COG_OK);
    assert(node != NULL);
    assert(!cog_session_is_empty(s));
    
    // Fork
    cog_session_node_t *fork = NULL;
    assert(cog_session_fork(s, node, &fork) == COG_OK);
    assert(fork != NULL);
    
    // History
    const cog_message_t *msgs = NULL;
    size_t count = 0;
    assert(cog_session_get_history(s, node, &msgs, &count) == COG_OK);
    assert(count == 1);
    
    // NULL handling
    assert(cog_session_add_message(NULL, NULL, &msg, NULL) == COG_ERROR_NULL_PTR);
    assert(cog_session_add_message(s, NULL, NULL, NULL) == COG_ERROR_NULL_PTR);
    
    cog_session_free(s);
    return 0;
}
```

**When**: After implementing each module, before marking it done.
**Who**: Module implementer writes it, critic reviews it.

### Level 3: Integration tests (cross-module)

Test that modules work together correctly:

```c
// test_core_session_integration.c
#include <cog/core.h>
#include <cog/session.h>
#include <assert.h>

int main(void) {
    // Create session
    cog_session_t *session = NULL;
    cog_session_new(&session, NULL);
    
    // Create core with session
    cog_core_config_t config = {
        .session = session,
        .dispatch_table = my_table,
        .dispatch_table_size = 1,
        // ... other required fields
    };
    cog_core_t *core = NULL;
    assert(cog_core_new(&core, &config) == COG_OK);
    
    // Enqueue event
    cog_event_t event = { .kind = COG_EVENT_USER_INPUT };
    assert(cog_core_enqueue(core, &event) == COG_OK);
    
    // Run one iteration
    // ... verify session was updated
    
    cog_core_free(core);
    cog_session_free(session);
    return 0;
}
```

**When**: After integrating two modules, before moving to the next phase.
**Who**: Integration tester (or architect if integration fails).

---

## Parallelization Rules

### Safe to parallelize

Modules that only depend on already-implemented modules:

```
Phase 3: cog-ipc, cog-obs, cog-config, cog-net
  All depend only on cog-core headers (already implemented)
  No interdependencies between them
  → Can be implemented in parallel by 4 agents
```

### Unsafe to parallelize

Modules that depend on each other or on unimplemented modules:

```
Phase 2: cog-core/core.h
  Depends on session, event, state, handler, ipc, obs, provider
  ipc and obs not yet implemented (Phase 3)
  → Must implement ipc and obs first, or use forward declarations
```

**Solution**: Use forward declarations and opaque pointers for Phase 2.
The core engine doesn't need to call ipc/obs functions — it just stores
pointers. The actual calls happen in Phase 3.

### Dependency matrix

| Module | Depends on | Can parallelize with |
|--------|-----------|----------------------|
| result.h | — | message.h |
| message.h | — | result.h |
| session.h | result, message | event.h |
| event.h | result | session.h |
| state.h | event, handler | — |
| handler.h | event, result | — |
| core.h | all above + ipc, obs, provider | — (must be last in Phase 2) |
| agent.h | session, result | tool.h, session_store.h |
| tool.h | event, result | agent.h, session_store.h |
| session_store.h | session, result | agent.h, tool.h |
| cog-ipc | result | cog-obs, cog-config, cog-net |
| cog-obs | result | cog-ipc, cog-config, cog-net |
| cog-config | result | cog-ipc, cog-obs, cog-net |
| cog-net | result | cog-ipc, cog-obs, cog-config |
| cog-provider | core | cog-toolkit |
| cog-toolkit | core | cog-provider |
| cog-orchestrator | ipc, core | — |

---

## Testing Strategy

### Unit tests (per module)

Each module gets a test file that exercises its public API:

```
tests/
  unit/
    test_result.c
    test_message.c
    test_session.c
    test_event.c
    test_state.c
    test_handler.c
    test_core.c
    test_agent.c
    test_tool.c
    test_session_store.c
    test_ipc.c
    test_obs.c
    test_config.c
    test_net.c
    test_provider.c
    test_toolkit.c
    test_orchestrator.c
```

**Coverage target**: Every public function called at least once.
**Error cases**: NULL, overflow, invalid state, not found.
**Memory**: ASan + UBSan clean.

### Integration tests (cross-module)

```
tests/
  integration/
    test_core_session.c      # core + session tree
    test_core_ipc.c          # core + IPC fabric
    test_core_obs.c          # core + observability
    test_core_provider.c     # core + provider registry
    test_core_toolkit.c      # core + tool execution
    test_orchestrator_ipc.c  # orchestrator + IPC
    test_full_agent.c        # end-to-end: user prompt → LLM → response
```

**When**: After each phase integration.
**Who**: Integration tester.

### Scenario tests (end-to-end)

The 20 scenario traces become executable tests:

```
tests/
  scenarios/
    test_01_user_prompt.c
    test_02_network_recovery.c
    test_03_subagent_lifecycle.c
    ...
applications/cog-cli/tests/
    test_11_ui_reconnect.c
    test_12_rate_limit.c
    ...
    test_20_interrupt_and_queue.c
```

Each test:
1. Sets up the scenario (create core, session, provider, etc.)
2. Executes the sequence from the trace
3. Verifies each step produces the expected events
4. Verifies the final state matches the trace

**When**: After all modules are implemented.
**Who**: QA / final validation.

---

## Contract Enforcement

### The contract file

Each module has a `CONTRACT.md` that lists:

1. **Public types** — every struct, enum, typedef
2. **Public functions** — every function signature
3. **Error codes** — every return value and when it's returned
4. **Threading** — what's thread-safe, what's not
5. **Ownership** — who allocates, who frees
6. **Invariants** — what must always be true

Example for `session.h`:

```markdown
# session.h Contract

## Types
- cog_session_t (opaque)
- cog_session_node_t (opaque)
- cog_session_config_t (public)
- cog_timing_t (public)
- cog_cost_t (public)
- cog_node_role_t (enum)
- cog_node_data_t (public)

## Functions
- cog_session_new(out, config) → COG_OK | COG_ERROR_NULL_PTR | COG_ERROR_NOMEM
- cog_session_free(session) → void
- cog_session_add_message(session, parent, msg, out_node) → COG_OK | COG_ERROR_NULL_PTR | COG_ERROR_NOMEM
- ...

## Threading
- Not thread-safe
- Owned by one core instance

## Ownership
- Session owns all nodes
- Caller owns the session
- cog_session_free destroys everything

## Invariants
- Root node has parent_id == 0
- Every non-root node has a valid parent
- The active node is always a leaf
- Forked branches share immutable ancestors
```

### Verification

The contract test verifies every claim in CONTRACT.md:

```c
// Verify: "Root node has parent_id == 0"
cog_session_node_t *root = cog_session_root(session);
const cog_node_data_t *data = cog_node_data(root);
assert(data->parent_id == 0);

// Verify: "Every non-root node has a valid parent"
cog_session_node_t *child = cog_node_first_child(root);
const cog_node_data_t *child_data = cog_node_data(child);
assert(child_data->parent_id == data->node_id);
```

---

## Git Workflow

### Branch strategy

```
main
  ├── impl/result-message      (Phase 0)
  ├── impl/session             (Phase 1)
  ├── impl/event               (Phase 1)
  ├── impl/state-handler       (Phase 1)
  ├── impl/core-engine         (Phase 2)
  ├── impl/ipc                 (Phase 3)
  ├── impl/obs                 (Phase 3)
  ├── impl/config              (Phase 3)
  ├── impl/net                 (Phase 3)
  ├── impl/provider            (Phase 4)
  ├── impl/toolkit             (Phase 4)
  ├── impl/orchestrator        (Phase 5)
  └── impl/cli                 (Phase 6)
```

### Merge requirements

A module branch can only merge to main when:

1. All unit tests pass
2. All integration tests with already-merged modules pass
3. Contract test passes
4. Critic review passes
5. No regressions in existing tests

### Commit message format

```
impl(<module>): <what was implemented>

- Added <file>.c with <functions>
- Contract test: tests/unit/test_<module>.c
- Integration test: tests/integration/test_<module>_<other>.c
- All tests pass, ASan clean

Ticket: <tix-id>
```

---

## Timeline Estimate

| Phase | Modules | Estimated effort | Parallelism |
|-------|---------|-----------------|-------------|
| 0 | result, message | 1 day | 2 agents |
| 1 | session, event, state, handler | 3 days | 4 agents |
| 2 | core, agent, tool, store | 5 days | 1 agent (sequential) |
| 3 | ipc, obs, config, net | 4 days | 4 agents |
| 4 | provider, toolkit | 3 days | 2 agents |
| 5 | orchestrator | 2 days | 1 agent |
| 6 | cli | 2 days | 1 agent |
| — | Integration & scenario tests | 3 days | 2 agents |
| **Total** | | **23 days** | |

---

## Risk Mitigation

### Risk: Contract drift during parallel implementation

**Mitigation**: Contract tests are written *before* implementation. The test
is the contract. If the implementation doesn't pass the test, the
implementation is wrong (not the test).

### Risk: Integration failures

**Mitigation**: Integration tests run after every merge. A failing integration
test blocks the next phase.

### Risk: Circular dependencies in implementation

**Mitigation**: The dependency matrix is explicit. A module can only include
headers from modules in earlier phases or the same phase (if no dependency).

### Risk: Memory bugs

**Mitigation**: ASan + UBSan on every test. Valgrind on integration tests.
No manual memory management without a matching free.

---

## Definition of Done

A module is done when:

- [ ] All public functions implemented
- [ ] All error codes returned correctly
- [ ] Unit test passes (every function called)
- [ ] Contract test passes (every claim verified)
- [ ] Integration test with dependencies passes
- [ ] ASan + UBSan clean
- [ ] Critic review approved
- [ ] Merged to main
- [ ] Ticket marked done

The project is done when:

- [ ] All 24 modules implemented
- [ ] All 20 scenario tests pass
- [ ] Full end-to-end: user prompt → LLM → streaming → UI
- [ ] Documentation updated
- [ ] No open tickets
