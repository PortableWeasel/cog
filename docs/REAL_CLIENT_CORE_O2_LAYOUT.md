# O2 private core/agent implementation passes

Parent99c42ec, core sourcee55f22c, agent source959efdc. Exact O base79c8dbc
includes approved IPC/auth and read-only readiness prerequisite5fcaef7. Public
N core/agent/event headers remain the contract; this is private implementation
layout, not another public architecture or permission to edit app/orchestrator.

## Pass1 — owners and one driver engine

core_internal.h relocates the existing core.c layout and adds one drive-state
record, persistent checked-clock sample, constructor step ceiling and per-peer
metadata. Driver kinds NONE/RUN/MANUAL prevent competing ownership. inside_slice
blocks callback recursion; done/result latch stop/first fatal error until end.
The run wrapper begins the same private engine, repeatedly executes bounded
slices, and invokes legacy wait/idle/nanosleep only outside slices. Public manual
step/end reject RUN ownership; all invalid calls preserve outputs/state.

Begin changes no queue/state/session leases, resets stop exactly at entry,
marks service due and publishes running. A slice dispatches at most the configured
step_event_limit (default16,max1024); service interval, deadlines and local/IPC
rotation persist across slices. Clock sampling uses CLOCK_MONOTONIC with checked
seconds/nanoseconds/arithmetic and nondecreasing samples between bounded units.
Only running and stop_requested remain atomics; stop stays the sole signal/
cross-thread operation. Other state is owner-thread only.

One outcome-finalization helper latches fatal/stop and clears running. One
ownership-ending helper restores the unprocessed injected FIFO reservation once,
logs the existing stop/error and makes the core inactive even on work failure.
Queued intents stay leased until dispatch or explicit teardown; budget yield is
not an acknowledgment. A popped event is dispatched once and its exact owned
IPC/intent lease is released on every handler/save/error path. Free discards
queued intent descriptors without executing them before registry destruction;
caller still fences/drains work and returns child borrows before destruction.

## Peer identity and queue ownership

Each accepted tracking slot embeds endpoint,id,by-value proof and CONNECTED
publication state. The transferred attached connected endpoint has a separate
embedded slot; no extra runtime allocation. Accepted tracking remains constructor
bounded. Query/capture happens before first service for an already-ready endpoint,
then immediately after each one-unit handshake publication before another unit,
regardless of queue capacity. Unsupported/query failure refuses unproven peers
without trusted events. Protected CONNECTED waits for proof and event space.

After proof capture, complete retained frames can be copied after EOF using
cached proof, not a fresh live-socket query. Each event copies the proof through
its handler lease; peer removal cannot invalidate it. Public core_enqueue rejects
IPC descriptors claiming peer_info_valid, preserving generic proof=false IPC.
Exact listener fabric/endpoint lifetime is the cache key, never numeric ID alone.

agent_internal.h adds execution_borrowed to the existing registry entry. Local,
nonterminal, uncancelled records lend exactly entry.session; no new tree, copied
ID substitute, allocation, update/ACK, effect or intent consumption. Already
borrowed/remote/terminal records refuse; admitted cancellation refuses ABORTED.
Return requires the exact record/pointer and completed child cleanup; it only
clears the lifetime lease. Terminal release also refuses an outstanding borrow.
Registry destruction requires all borrows returned, never an implicit child join.
