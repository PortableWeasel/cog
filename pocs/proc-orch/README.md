# PoC 2 — Process orchestration

This proof-of-concept proves the main Cog core can spawn and monitor
auxiliary core processes. It is the direct follow-up to
[PoC 1](../ipc-fabric/README.md), reusing its 4-byte-big-endian framing
scheme.

## What it proves

- A main process `fork(2)`+`execv(2)`es **N** auxiliary-core child
  processes, each connected to the main process by a dedicated
  `socketpair(AF_UNIX, SOCK_STREAM)`.
- Children send **periodic heartbeats** to the main process.
- The main process **detects a child exit** (socket EOF + `waitpid`
  reaping) and a **heartbeat timeout** (no heartbeat for 3x the
  interval) while the child process is still alive.
- The main process sends a **`spawn subagent <id>` request** to a child
  and the child **acknowledges** with `ack <id>`, proving two-way
  request/acknowledge flow from a spawned core.

## Architecture

A single executable, `proc-orch`, plays both roles. The orchestrator
spawns auxiliary cores by re-exec'ing itself (`/proc/self/exe`) in
worker mode, so no separate worker binary needs to be shipped:

```
proc-orch                                     (orchestrator / main)
  └─ fork + execv(/proc/self/exe)  ──────────  proc-orch worker --id 1 --fd N
    └─ fork + execv(/proc/self/exe)  ────────  proc-orch worker --id 2 --fd N
      └─ fork + execv(/proc/self/exe)  ──────  proc-orch worker --id 3 --fd N --lazy-after 4
```

Each child inherits one end of a fresh `socketpair`; the fd survives
`execv` because it is never marked `FD_CLOEXEC`. The child closes every
other inherited descriptor before exec'ing, so workers do not leak the
orchestrator's other connections. `/proc/self/exe` is Linux-specific and
intentional here (the project targets Linux; a real spawner would take
an explicit binary path).

### Timeline (single thread, poll(2)-driven)

The orchestrator never sleeps; everything is driven by one `poll(2)`
loop:

1. Spawn N children (default 3).
2. Wait until every child has heartbeated at least once.
3. Send `spawn subagent 1001` to child 1, wait for `ack 1001`.
4. SIGTERM child 2 → detect its socket EOF, reap it, log the exit.
5. Child 3 was spawned with `--lazy-after 4`: it heartbeats four times
   then goes quiet while staying alive → the orchestrator logs a
   heartbeat timeout for it.
6. SIGTERM the survivors, reap everything, clean up.

### Heartbeat/timeout model

- Heartbeat interval: 250 ms by default (`COG_ORCH_INTERVAL_MS`,
  50–10000).
- Timeout: 3x the interval (750 ms by default), measured on
  `CLOCK_MONOTONIC`.
- The poll timeout is computed as the nearest heartbeat deadline, so the
  loop wakes exactly when a child is due to be declared dead; a heart
  beat observed before the deadline resets it.
- A timeout only applies once a child has heartbeated at least once,
  which avoids a startup race (a freshly exec'd child is given time to
  produce its first beat).

## Framing and protocol

Framing is identical to PoC 1: a 4-byte big-endian payload length
followed by the payload. PoC 2 caps messages at 1024 bytes because
control traffic is tiny; the cap is enforced by the frame reader, and a
violation drops the connection (`fail loud`).

Messages are newline-free text payloads:

| Direction | Message | Meaning |
|-----------|---------|---------|
| child → main | `heartbeat <child-id> <seq>` | liveness report |
| main → child | `spawn subagent <subagent-id>` | workload assignment |
| child → main | `ack <subagent-id>` | acknowledgment of assignment |

## Building

From the repository root:

```bash
cmake -B build -DCOG_BUILD_POC_PROC_ORCH=ON
cmake --build build
```

This produces `build/pocs/proc-orch/proc-orch`. Build with
`-DCOG_ENABLE_ASAN=ON` for a sanitizer build and `-DCOG_ENABLE_WERROR=ON`
for a warnings-as-errors check. The code is C11, `-Wall -Wextra
-Wpedantic -Wconversion` clean, and uses `_POSIX_C_SOURCE 200809L`.

## Running

```bash
./build/pocs/proc-orch/proc-orch            # N=3, one complete demo
COG_ORCH_CHILDREN=1 ./proc-orch             # N=1: spawn + ack only
COG_ORCH_INTERVAL_MS=100 ./proc-orch        # faster heartbeat cadence
```

The orchestrator prints one machine-readable event per line
(`heartbeat ...`, `spawn-sent ...`, `ack ...`, `exit ...`, `timeout ...`)
and exits non-zero if any required scenario step is missing.

The demo is registered as the `proc-orch-demo` CTest:

```bash
ctest --output-on-failure -R proc-orch
```

`demo-test.sh` asserts every acceptance event for the default N=3 run:
heartbeats from all three children, spawn/ack on child 1, exit
detection for child 2, heartbeat timeout for child 3, and a clean
finish.

## Acceptance check

- [x] Main process forks/execs N child processes (default 3, 1..16).
- [x] Children send periodic heartbeats to main.
- [x] Main detects child exit (SIGTERM'd child 2) and heartbeat timeout
      (lazy child 3, process still alive at detection).
- [x] Main sends a 'spawn subagent' request and the child acknowledges.

## Design decisions and notes

- **`socketpair` over pipes.** A `SOCK_STREAM` socketpair gives
  bidirectional, ordered byte streams with canonical EOF semantics for
  exit detection, and it reuses the PoC 1 framing on both directions.
- **fork+exec, not fork-only.** The ticket asks to prove fork *and*
  exec: an auxiliary core will be a real binary (possibly a different
  one), so the spawn path must survive an image replacement. The fd is
  passed by number on the command line after clearing CLOEXEC.
- **Single binary, two modes.** Re-exec'ing the current executable keeps
  the PoC self-contained and avoids a hard-coded worker path; the real
  spawner will take the target binary from configuration.
- **One poll loop, no threads.** Matches the project's single-core
  correctness-first stance; scaling to thousands of children would swap
  `poll` for `epoll` behind the same framing layer (as noted in PoC 1).
- **No global mutable state, visible allocations.** All state lives in
  a caller-allocated `struct orchestrator` / `struct worker`; dynamic
  buffers are free'd on every exit path.
- **Zombie-free.** Every child is reaped with `waitpid(WNOHANG)` in the
  main loop; a 30 s safety deadline aborts a wedged demo and tears
  everything down.
- **N < 3 degrades gracefully.** With one child only the spawn/ack step
  runs; with two, exit detection is added. The demo test uses the
  default N=3 so all steps are exercised.
