# Cog

An LLM harness and agent framework in C11: a table-driven state machine that
routes events (user input, tool results, signals) to handlers (provider calls,
tool execution, file I/O), validates results, and advances session state.
Think `pi` / `opencode`, but compiled and embeddable.

**Status: pre-alpha, Phase 6 application integration.** The eight libraries and
`cog-cli` have C11 implementations. Public headers remain the contract, and
`pocs/` contains standalone reference proofs rather than library code.

## The header-first method

We design the entire system as public headers before writing any implementation.
This is deliberate and non-negotiable.

### Why headers first

1. **The interface is the truth.** If the headers don't cover a scenario, the
   implementation will be wrong. Fix the headers first.
2. **Scenario traces catch gaps.** Walking through end-to-end flows (user
   prompt → LLM → streaming → UI) reveals missing functions, events, and types.
3. **Amendments are cheap.** Changing a header costs nothing. Changing an
   implementation after the fact costs everything.
4. **Multiple iterations are required.** We go through at least 3 full passes:
   - Pass 1: Write initial headers from the design doc
   - Pass 2: Trace all 20 scenarios, find gaps, amend headers
   - Pass 3: Review for consistency, completeness, and edge cases

### The iteration loop

```
┌─────────────┐     ┌─────────────┐     ┌─────────────┐
│   Write     │────▶│    Trace    │────▶│   Amend     │
│   headers   │     │  scenarios  │     │   headers   │
└─────────────┘     └─────────────┘     └─────────────┘
       ▲                   │                   │
       │                   ▼                   ▼
       │            ┌─────────────┐     ┌─────────────┐
       │            │  Find gaps  │     │  Commit &   │
       └────────────│  (missing   │     │  re-verify  │
                    │   APIs)     │     │             │
                    └─────────────┘     └─────────────┘
```

Repeat until all 20 scenarios pass with zero gaps, then do one more full
review pass before implementation.

### Scenario traces

We maintain 20 end-to-end scenario traces in `docs/scenarios/`. Each has:
- A sequence diagram showing the flow
- A header coverage table mapping every call to its declaring header
- A gap analysis listing what's missing

The 20 scenarios:
1. User prompt → LLM → streaming → UI
2. Network stall → detection → recovery → resume
3. Subagent spawn → execute → status → result → parent
4. Cold start → init → first event
5. Graceful shutdown with in-flight work
6. Parallel tool calls with timeout
7. Aux core crash → detection → recovery
8. Hot config reload
9. Subagent-to-subagent messaging across cores
10. Fan-out/fan-in across aux cores
11. UI disconnect → buffer → reconnect → replay
12. Provider rate limit → backoff → queue
13. Session persistence and recovery
14. Conversation fork and branch isolation
15. Multi-turn tool use
16. Provider error propagation
17. Session migration
18. Structured observability
19. Connection diagnostics and events
20. User interrupt and queued input

Every scenario has an executable test and must pass before the Phase 6 gate.
Scenarios 01–10 live in `tests/scenarios`; scenarios 11–20 live in
`applications/cog-cli/tests`.

## Layout

```
libraries/
  cog-core/       agent loop, session tree, event queue, state machine
  cog-provider/   LLM endpoint registry, provider vtable, model/capability
  cog-toolkit/    tool call/exec/result API
  cog-ipc/        Unix domain socket fabric, session routing, broadcast
  cog-config/     hierarchical config tree with commit/rollback
  cog-net/        HTTP client, streaming, stall detection, retry
  cog-obs/        logging, tracing, diagnostics
  cog-orchestrator/  process lifecycle, health, placement
    include/cog/  public headers (the contract)
    src/          private implementation
applications/cog-cli/  installed CLI, testable app library, scenarios 11–20
pocs/             standalone proofs for risky design questions
tests/critic-smoke/  header self-containment / strict-warning compile checks
docs/scenarios/   20 end-to-end validation traces
docs/diagram.svg  system block diagram
docs/DESIGN.md    system blueprint — read before changing architecture
.pi/              pi agent config and the cog-dev orchestrator workflow
```

Dependencies: `cog-provider` and `cog-toolkit` link `cog::core`. `cog-core`
depends on `cog-ipc`, `cog-obs`, and `cog-provider` (forward-declared types).
Keep the dependency graph acyclic.

## Build and test

```bash
cmake -S . -B build -DCOG_ENABLE_WERROR=ON
cmake --build build
ctest --test-dir build --output-on-failure -j4
ctest --test-dir build -L scenario --output-on-failure

# Sanitizers require separate build directories.
cmake -S . -B build-asan -DCOG_ENABLE_WERROR=ON -DCOG_ENABLE_ASAN=ON
cmake --build build-asan
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -DCOG_ENABLE_WERROR=ON -DCOG_ENABLE_TSAN=ON
cmake --build build-tsan
TSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-tsan --output-on-failure

# Stage and run the application.
cmake --install build --prefix install/stage
install/stage/bin/cog-cli --prompt hello

# Verify all headers compile standalone
for f in $(find libraries -name "*.h" | sort); do
  gcc -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
      -Wsign-conversion -Wmissing-prototypes -Wstrict-prototypes \
      -Wundef -Wpointer-arith -Wcast-align -Wwrite-strings \
      -Wlogical-op -Wformat=2 -Werror -c -x c "$f" -o /dev/null \
      -I libraries/cog-core/include \
      -I libraries/cog-provider/include \
      -I libraries/cog-toolkit/include \
      -I libraries/cog-ipc/include \
      -I libraries/cog-obs/include \
      -I libraries/cog-config/include \
      -I libraries/cog-net/include \
      -I libraries/cog-orchestrator/include
done
```

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
7. **Single-threaded first.** One agent loop, one thread. `cog_core_stop` is
   the only call intended to be safe from another thread or a signal handler.

## Conventions

- Namespace: `cog_` for functions, `COG_` for macros/constants, `_t` for types.
  Headers live under `include/cog/`, guard `COG_<NAME>_H`, wrapped in
  `extern "C"`, documented with Doxygen (`@file`, `@brief`, `@param`).
- Headers must compile standalone and warning-free under the strict flags.
- CMake targets are `cog-<name>` with an alias `cog::<name>`, install rules
  and an exported `cog-<name>-targets` set under namespace `cog::`.
- Config is a hierarchical dotted-path tree (`providers.openai.base-url`) with
  candidate/commit/rollback semantics; see `pocs/config-tree`.
- Sessions are hierarchical trees with forking, timing, and billing;
  see `libraries/cog-core/include/cog/session.h`.
- Persistence uses a pluggable backend (static dispatch table);
  see `libraries/cog-core/include/cog/session_store.h`.

## Proofs of concept

`pocs/` holds standalone programs that answer a design question with running
code. Each has its own README and is off by default behind a
`COG_BUILD_POC_*` option.

| PoC | Question it answers |
|-----|---------------------|
| `ipc-fabric` | Core/UI split over Unix domain sockets |
| `proc-orch` | Multi-process orchestration, reader overflow |
| `http-streaming` | HTTP + SSE streaming, connection tracking, stall detection |
| `config-tree` | Candidate/commit/rollback config tree |

PoC code is reference material, not library code: port ideas into
`libraries/`, don't link against `pocs/`.

## Working here

- **Headers before implementation.** Always. No exceptions.
- **Trace scenarios to find gaps.** Every new feature gets a scenario trace.
- **Iterate at least 3 times.** The first design is never right.
- **Read `docs/DESIGN.md` first** for anything touching state machine,
  provider registry, tool execution, or config. Update it in the same commit
  when a decision changes.
- **Commits explain why**, not just what. Style: `type(scope): summary`
  (`feat(core)`, `fix(cog-toolkit)`, `test(critic-smoke)`, `design(cog-provider)`,
  `build(...)`).
- **Track work with `tix`.** Every design decision, gap fix, and implementation
  task gets a ticket. Tickets are the project memory.
