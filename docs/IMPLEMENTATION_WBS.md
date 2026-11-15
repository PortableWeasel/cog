# Cog — Implementation Work Breakdown Structure

## Overview

37 active tickets organized into 7 phases with 7 gates. Each gate must
pass before the next phase begins.

**Root ticket:** `96d36f6` — Stage 12: Implementation
**Hard start gate:** `cff35d0` — must be manually marked done before any work

---

## Phase 0: Foundation (result + message)

**Ticket:** `9d5b705` — Stage 12.0: Foundation
**Gate:** `7b636e6` — GATE 12.0: Phase 0 complete

| Sub-task | Ticket | Description |
|----------|--------|-------------|
| (none — header-only) | — | result.h and message.h are header-only |

**Gate checklist:**
- [ ] test_result.c compiles and passes
- [ ] test_message.c compiles and passes
- [ ] Both headers compile standalone with -Werror
- [ ] CONTRACT.md exists for both
- [ ] No ASan/UBSan errors

**Blocks:** Phase 1

---

## Phase 1: Core Primitives

**Ticket:** `8ba8329` — Stage 12.1: Core primitives
**Gate:** `1555b57` — GATE 12.1: Phase 1 complete

| Sub-task | Ticket | Description |
|----------|--------|-------------|
| session.c | `0f055f5` | Conversation tree with forking, timing, billing |
| event.c | `2875a5a` | Event queue |
| state.c | `6b1c97b` | State machine |
| handler.c | `9298184` | Handler dispatch |

**Parallel:** 4 agents can work simultaneously
**Dependency:** Gate 0

**Gate checklist:**
- [ ] All 4 modules implemented
- [ ] All unit tests pass
- [ ] All contract tests pass
- [ ] ASan + UBSan clean
- [ ] session.c: fork/history/discard work correctly
- [ ] event.c: queue overflow handled
- [ ] state.c: all transitions valid
- [ ] handler.c: errors propagated

**Blocks:** Phase 2

---

## Phase 2: Core Engine

**Ticket:** `eb5475c` — Stage 12.2: Core engine
**Gate:** `9b130b7` — GATE 12.2: Phase 2 complete

| Sub-task | Ticket | Description |
|----------|--------|-------------|
| core.c | `681a566` | Agent loop, event dispatch |
| agent.c | `6a025eb` | Subagent lifecycle |
| tool.c | `62a438e` | Tool table |
| session_store.c | `0efef06` | Persistence |

**Parallel:** Sequential (core.c depends on all)
**Dependency:** Gate 1

**Gate checklist:**
- [ ] All 4 modules implemented
- [ ] All unit tests pass
- [ ] Integration test: core + session passes
- [ ] ASan + UBSan clean
- [ ] Event loop processes events correctly
- [ ] Session persistence round-trip works
- [ ] Queue/dequeue works

**Blocks:** Phase 3

---

## Phase 3: Infrastructure

**Ticket:** `c21780e` — Stage 12.3: Infrastructure
**Gate:** `decf729` — GATE 12.3: Phase 3 complete

| Sub-task | Ticket | Description |
|----------|--------|-------------|
| ipc.c | `4de26fd` | Unix sockets, session routing |
| obs.c | `7f5b166` | Logging, tracing, diagnostics |
| config.c | `df4d0fa` | Config tree |
| net.c | `92e883f` | HTTP client, streaming |

**Parallel:** 4 agents can work simultaneously
**Dependency:** Gate 2

**Gate checklist:**
- [ ] All 4 modules implemented
- [ ] All unit tests pass
- [ ] Integration tests: core+ipc, core+obs pass
- [ ] ASan + UBSan clean
- [ ] IPC: send/receive works
- [ ] Obs: logging and tracing work
- [ ] Net: HTTP streaming works
- [ ] Config: commit/rollback works

**Blocks:** Phase 4

---

## Phase 4: Provider & Toolkit

**Ticket:** `369009f` — Stage 12.4: Provider & Toolkit
**Gate:** `0404fd6` — GATE 12.4: Phase 4 complete

| Sub-task | Ticket | Description |
|----------|--------|-------------|
| provider.c | `80e64c5` | LLM abstraction |
| toolkit | `8d52c1a` | Tool execution framework |

**Parallel:** 2 agents (provider, toolkit)
**Dependency:** Gate 3

**Gate checklist:**
- [ ] Provider implemented
- [ ] Toolkit implemented
- [ ] All unit tests pass
- [ ] Integration tests pass
- [ ] ASan + UBSan clean
- [ ] Provider: streaming works
- [ ] Toolkit: parallel execution works

**Blocks:** Phase 5

---

## Phase 5: Orchestrator

**Ticket:** `3e6262c` — Stage 12.5: Orchestrator
**Gate:** `d064787` — GATE 12.5: Phase 5 complete

| Sub-task | Ticket | Description |
|----------|--------|-------------|
| orchestrator.c | (part of phase) | Process lifecycle |

**Parallel:** Sequential (depends on ipc)
**Dependency:** Gate 4

**Gate checklist:**
- [ ] Orchestrator implemented
- [ ] Unit tests pass
- [ ] Integration test passes
- [ ] ASan + UBSan clean
- [ ] Spawn/health/shutdown work

**Blocks:** Phase 6

---

## Phase 6: Application (CLI)

**Ticket:** `81b1227` — Stage 12.6: Application
**Gate:** `04b28bf` — GATE 12.6: Phase 6 complete

| Sub-task | Ticket | Description |
|----------|--------|-------------|
| main.c | (part of phase) | CLI application |
| Scenario tests | (part of phase) | All 20 scenarios |

**Parallel:** Sequential (final integration)
**Dependency:** Gate 5

**Gate checklist:**
- [ ] CLI implemented
- [ ] All 20 scenario tests pass
- [ ] ASan + UBSan clean
- [ ] Full end-to-end: prompt → LLM → streaming → UI
- [ ] Documentation updated

**Project complete when this gate passes.**

---

## Dependency Graph

```
cff35d0 (GATE: hard start)
    │
    ▼
96d36f6 (Stage 12: Implementation)
    │
    ├── 9d5b705 (Phase 0: Foundation)
    │       │
    │       ▼
    │   7b636e6 (GATE 0)
    │       │
    │       ▼
    ├── 8ba8329 (Phase 1: Core Primitives)
    │   ├── 0f055f5 (session.c)
    │   ├── 2875a5a (event.c)
    │   ├── 6b1c97b (state.c)
    │   └── 9298184 (handler.c)
    │       │
    │       ▼
    │   1555b57 (GATE 1)
    │       │
    │       ▼
    ├── eb5475c (Phase 2: Core Engine)
    │   ├── 681a566 (core.c)
    │   ├── 6a025eb (agent.c)
    │   ├── 62a438e (tool.c)
    │   └── 0efef06 (session_store.c)
    │       │
    │       ▼
    │   9b130b7 (GATE 2)
    │       │
    │       ▼
    ├── c21780e (Phase 3: Infrastructure)
    │   ├── 4de26fd (ipc.c)
    │   ├── 7f5b166 (obs.c)
    │   ├── df4d0fa (config.c)
    │   └── 92e883f (net.c)
    │       │
    │       ▼
    │   decf729 (GATE 3)
    │       │
    │       ▼
    ├── 369009f (Phase 4: Provider & Toolkit)
    │   ├── 80e64c5 (provider.c)
    │   └── 8d52c1a (toolkit)
    │       │
    │       ▼
    │   0404fd6 (GATE 4)
    │       │
    │       ▼
    ├── 3e6262c (Phase 5: Orchestrator)
    │       │
    │       ▼
    │   d064787 (GATE 5)
    │       │
    │       ▼
    └── 81b1227 (Phase 6: Application)
            │
            ▼
        04b28bf (GATE 6 — PROJECT COMPLETE)
```

---

## Ticket Count

| Type | Count |
|------|-------|
| Phases | 7 |
| Sub-tasks | 14 |
| Gates | 7 |
| Root | 1 |
| Hard start gate | 1 |
| **Total** | **30** |

(Plus 7 old cancelled tickets = 37 total)

---

## How to Start

1. Review all design documents
2. Verify all 24 headers compile
3. Verify all 20 scenarios are documented
4. When ready: `tix edit cff35d0 --status done`
5. Phase 0 becomes available
6. Complete Phase 0 → Gate 0 → Phase 1 → Gate 1 → ...

---

## How to Check Status

```bash
# View WBS
tix wbs 96d36f6

# View all active tickets
tix ls --all | grep -v cancelled | grep -v done

# View next available work
tix claim --next

# View dependencies
tix show <ticket-id>
```
