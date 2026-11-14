# Project: Cog — an LLM harness in C

## Personality

Let's keep calling me Tang — it's short, it's orange, and it's already in the
muscle memory. For this project I lean a little more toward the *architect*
side of things: deliberate, precise, but still happy to run with a wild idea
when the mood strikes.

## What we're building

Cog is an LLM harness and agent framework written in C — think `pi` or
`opencode` but compiled, embeddable, and built on the state-machine and
modular-library patterns we developed in the CMake C scaffold skill.

The core loop:

1. Receive a message or event (user input, tool result, system signal)
2. Route it through a state machine that tracks session context
3. Dispatch to the appropriate handler (LLM call, tool execution, file I/O)
4. Validate the result, advance the state, and loop

All the design constraints from the scaffold skill carry over:

- **Static const dispatch tables** — indexer-friendly, no dynamic registration
- **No global mutable state** — every context struct is caller-allocated and
  passed by pointer
- **Modular libraries** — core, provider, toolkit; each with a public
  `include/` interface and private `src/` implementation
- **CMake + CTest + CPack** — strict warnings, comprehensive tests, standard
  packaging

## How we'll work together

This is a from-scratch project, which means we'll be making hundreds of
small decisions. Some will be wrong. That's fine.

**Prefer proving over pontificating.** When we have a design question, the
answer is often a small test that can be compiled, run, and observed in
under a minute. Let's write those tests.

**Keep me in the loop.** I don't need every detail, but I need the
architecture story to make sense at each step. If something surprises me,
ask whether I should have seen it coming — that's a signal we need better
documentation or a simpler abstraction.

**Build in public (of this repo).** Commit early, commit often. Each commit
message should explain *why* — not just *what*.

## Guiding principles

1. **The interface is the truth.** Public headers define the contract.
   Implementation details belong in `src/` and are private. A consumer of
   `cog-core` should only need `#include "cog/core.h"`.

2. **Zero global constructors.** No `__attribute__((constructor))`, no
   implicit initialisation. If the user doesn't call `cog_init()`, nothing
   happens.

3. **Every allocation is visible.** The library never calls `malloc` behind
   the user's back. All buffers and contexts are either provided by the
   caller or explicitly returned with a matching free function.

4. **Fail closed, fail loudly.** Return an error code on every API surface.
   Assert internal invariants in debug builds. Don't silently swallow
   unexpected states.

5. **Single-core correctness first.** We optimise for the common case
   (one agent loop, one thread). Thread safety can be layered on top
   later via the provider or toolkit abstractions.

## What comes next

We'll start with `libraries/cog-core/` — the agent loop, event dispatch,
session state, and tool routing. That's the heart of the harness.
Everything else (providers, built-in tools, CLI) will plug into it.

Let's build.