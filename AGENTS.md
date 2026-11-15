# Cog

An LLM harness and agent framework in C11: a table-driven state machine that
routes events (user input, tool results, signals) to handlers (provider calls,
tool execution, file I/O), validates results, and advances session state.
Think `pi` / `opencode`, but compiled and embeddable.

**Status: pre-alpha.** The three libraries are *header-only INTERFACE targets*
(public API headers only, no `src/` yet). The CLI is not wired in. Working code
currently lives in `pocs/`.

## Layout

```
libraries/
  cog-core/       agent loop, session, event queue, state machine, tool routing
  cog-provider/   LLM endpoint registry, provider vtable, model/capability table
  cog-toolkit/    tool call/exec/result API (built-in tools, shell, file ops)
    include/cog/  public headers (the contract)
    src/          private implementation (not created yet)
applications/     cog-cli goes here (add_subdirectory is commented out)
pocs/             standalone proofs for risky design questions (see below)
tests/critic-smoke/  header self-containment / strict-warning compile checks
cmake/            compiler_warnings.cmake (`target_enable_warnings(tgt)`)
docs/DESIGN.md    system blueprint — read before changing architecture
.pi/              pi agent config and the cog-dev orchestrator workflow
```

Dependencies: `cog-provider` and `cog-toolkit` link `cog::core` (they include
`<cog/result.h>` etc.). `cog-core` depends on nothing. Keep it that way.

## Build and test

```bash
cmake -S . -B build                       # Debug by default
cmake --build build
ctest --test-dir build --output-on-failure

# Useful options
-DCOG_ENABLE_ASAN=ON                      # ASan + UBSan
-DCOG_ENABLE_WERROR=ON                    # warnings are errors
-DCOG_BUILD_POC_CONFIG_TREE=ON            # also: _IPC_FABRIC, _HTTP_STREAMING, _PROC_ORCH
```

Strict warnings (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion
-Wsign-conversion -Wmissing-prototypes ...`) are applied per target via
`target_enable_warnings()`. New targets must call it. `CMAKE_C_EXTENSIONS` is
OFF: write ISO C11, not GNU C.

`tests/critic-smoke/*.c` are **not yet registered with CTest**. Each is a
tiny translation unit that includes one public header alone to prove it is
self-contained. Check one by hand with:

```bash
cc -std=c11 -Wall -Wextra -Wpedantic -Ilibraries/cog-core/include \
   -Ilibraries/cog-provider/include -Ilibraries/cog-toolkit/include \
   -fsyntax-only tests/critic-smoke/test_core_header.c
```

When you add a public header, add a matching `test_<name>_selfcontained.c`.

## Design constraints (non-negotiable)

1. **The interface is the truth.** Public headers in `include/cog/` define the
   contract; a consumer of cog-core needs only `#include "cog/core.h"`.
   Implementation details go in `src/` and stay private.
2. **Static const dispatch tables.** `(state, event) -> handler` and the
   provider/toolkit tables are `static const` data: indexer-friendly, no
   dynamic registration.
3. **No global mutable state.** Every context struct is caller-allocated and
   passed by pointer.
4. **Zero global constructors.** No `__attribute__((constructor))`, no implicit
   init. If the user doesn't call `cog_init()`, nothing happens.
5. **Every allocation is visible.** The library never mallocs behind the
   caller's back. Buffers are caller-provided, or returned by a constructor
   with a matching destructor. Output buffers take an explicit capacity
   parameter (see commit 5f78104 for why).
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

- **Prefer proving over pontificating.** For a design question, write a small
  test or PoC that compiles and runs in under a minute.
- **Read `docs/DESIGN.md` first** for anything touching state machine,
  provider registry, tool execution, or config. Update it in the same commit
  when a decision changes.
- **Commits explain why**, not just what. Style so far: `type(scope): summary`
  (`feat(core)`, `fix(cog-toolkit)`, `test(critic-smoke)`, `design(cog-provider)`,
  `build(...)`).
- Next up: `src/` implementations, starting with `cog-core` (loop, event
  dispatch, session state, tool routing); then provider, toolkit, and the CLI.
