# O2 private core/agent implementation passes

Parent99c42ec, core sourcee55f22c, agent source959efdc. Exact O base79c8dbc
includes approved IPC/auth and read-only readiness prerequisite5fcaef7. Public
N core/agent/event headers remain the contract; this is private implementation
layout, not another public architecture or permission to edit app/orchestrator.

## Pass1 — owners and one driver engine

core_internal.h relocates the existing core.c layout and adds one drive-state
record, persistent checked-clock sample, constructor step ceiling and per-peer
metadata. Driver kinds NONE/RUN/MANUAL prevent competing ownership. inside_call
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

## Pass2 — all20 traces and lifecycle amendments

| Scenario | O2 boundary checked |
|---|---|
|01 prompt|same static dispatch, successful transition/save, pending injection and IPC/local rotation in run and step|
|02 stall|service retains its own deadline; finite idle result never waits inside step|
|03 child|spawn intent lease admits one exact local session borrow; child cleanup precedes return|
|04 cold start|no dispatch/wait/allocation in begin; first slice services before first dispatch|
|05 shutdown|stop checked between units; stopped driver ends once; host drains external work|
|06 parallel tools|one single-thread owner per core; separate cores rotate bounded slices|
|07 aux failure|capture proof before next service, retained complete frame before EOF disposal|
|08 reload|same borrowed provider setter/whole-turn ownership; no new provider lifetime transfer|
|09 messages|copied kernel proof is fabric-scoped, generic external enqueue cannot forge it|
|10 fan-out|queued spawn/cancel leases survive budget/error and discard without executing at free|
|11 reconnect|listener accepted slots/proof move together; replacement links never inherit cache|
|12 backoff|elapsed checked clock and due deadlines survive slice boundaries; no reset per step|
|13 persistence|same exact save timing/error; drive end does not imply durable recovery or work drain|
|14 fork|existing session/branch state preserved across begin/end; no injected item retargeting|
|15 tool round|handler error releases popped event/intent/IPC lease once; no retry of effects|
|16 errors|first fatal work result latched, no subsequent dispatch, end consumes ownership even on error|
|17 migration|local child borrows block parent record release until child teardown; no pointer migration|
|18 observability|same run-start/stop/error labels at driver boundaries, no false per-slice restarts|
|19 connection facts|identity remains kernel establishment snapshot, never refreshed native liveness|
|20 interrupt|only lock-free existing atomic stop/running fields cross thread/signal boundary|

R13/R14 retain the same proof/queue/attempt restrictions. O2 adds neither remote
execution nor application role authentication; those remain orchestrator/app work.

### Exact first-proof and pressure ordering

A newly accepted record initializes proof=false, connected_queued=false. An
already-connected transferred endpoint uses an embedded record and preserves the
existing attached-endpoint path (no newly invented CONNECTED notification);
its connected_queued starts true, but protected MESSAGE still requires proof.
Before any service on an already-ready link, cache its native proof. Otherwise
drive exactly one handshake unit, then capture immediately if its nonzero ID and
published handshake permit query. Do not place an event/arena-capacity check
between that unit and capture. A client whose assigned ID is not yet published
is still pending; do not query zero and misclassify INVALID_ARG as native failure.

After capture, a full event queue may defer CONNECTED, but it cannot defer proof.
Complete RX and EOF can arrive meanwhile; CONNECTED then MESSAGE then DISCONNECTED
are published in the existing retained order when capacity permits. Each copied
descriptor owns proof by value. A failed/unsupported native query refuses that
peer without forged CONNECTED/DISCONNECTED proof. Actual Q PROTOCOL/LIMIT remains
a fatal driver result under N A48, not a new per-peer isolation policy; ordinary
EOF/IO follows existing peer cleanup. Service and receive results stay distinct
so an AGAIN due to arena pressure cannot erase a propagated fatal framing error.

### Rebegin and disposal

Amendment: driver end/begin resets ONLY driver-owned scheduling/stop state, not
peer proof or CONNECTED publication state. A stop before frame dispatch must not
lose proof for already-retained RX or duplicate CONNECTED on rebegin. Proof ends
when its endpoint record is removed; event copies remain valid afterward. A
listener peer ID is unique only during listen→disconnect. Endpoint ownership
transferred at successful core_new forbids callers relistening behind the core;
no stronger new→free identity is promised. If core itself retires its attached
endpoint, its metadata is cleared together with disabling that ingress path.

A popped event records whether it came from the private IPC queue. Only such an
owned arena slice is released; a generic external IPC-shaped descriptor does not
acquire arena ownership merely by carrying a matching-looking pointer. All agent
intents enter only through the existing private leased path; free drains queued
intents through lease-release bookkeeping before destroying their records.
Free requires inactive driver and returned session borrows, as debug invariants,
without pretending to cancel or join any externally owned operation.

### Borrow/return and readiness details

Return checks exact record/pointer/current borrow; an attached child core is a
detectable unfinished-cleanup violation and returns INVALID_STATE. An unattached
session alone does not prove provider/tool/guard cleanup; caller retains that
precondition. Return changes no terminal snapshot, notification ACK, intent lease
or one-shot task state. Borrow again after a valid return is only another storage
lease if otherwise eligible, never automatic task re-execution authorization.

Readiness combines queued descriptors, pending injection, due service/deadline
and the approved read-only IPC helper. A budget/fairness yield with actionable
work returns max_wait_ms0 and deadline<=sampled now. Idle writable sockets alone
do not count. Any transient readiness-query AGAIN gives a bounded immediate
retry; permanent query/clock errors latch normal fatal driver failure. Only the
run wrapper waits; manual slices never invoke wait_fn/idle_fn/nanosleep/poll>0.

## Pass3 — final consistency and executable acceptance plan

Rewalked pass2 against public core.h/agent.h/event.h, actual Q queue/arena leases,
O1's one-unit identity barrier and integrated read-only readiness. No public gap
or additional IPC body change is proposed. The private core header is C11-only
because its actual state contains C atomics; installed public headers retain
normal C/C++ support. Agent private layout remains C/C++ compatible.

Amendment: inside_call guards lifecycle entry, not just dispatch slices. Begin/
end observation callbacks must not recursively step/end/begin while ownership
is being published/consumed. Begin validates first, sets this guard before any
observer callback, then returns with a published owner and guard cleared. Step
publishes a temporary output only after valid work, clears the guard on every
return, and never changes invalid-call output. End restores the injected lease
once under the guard before releasing owner kind. Destruction also guards its
callbacks while consuming an inactive core; it does not silently end a live
manual drive. Existing atomic stop stays usable throughout.

One shared engine owns dispatch/transition/save, pending injection, service/
deadline fairness, proof ingress, error/stop latching and lease cleanup. The run
wrapper owns only repeated slices plus checked idle waiting. Budget counters
cannot wrap: the bounded dispatch count is checked before another dispatch, and
service counts reset at the configured interval before increment can overflow.
Clock seconds multiplication, nanoseconds range, addition and nondecreasing
sample are checked; failure is IO and never synthesizes time. Core clock history
survives rebegin, while first service is due on each new drive.

Constructor arithmetic checks the enlarged peer-record array and all existing
reservations before allocation; transferred endpoint/session ownership changes
only on successful construction. Peer-cache state is per core, not global. No
proof/credential or borrowed pointer is reconstructed from session text, wire
HELLO or coincident numeric IDs. No dynamic dispatch/registration/growing arena
is introduced. Agent entry destruction asserts returned borrows in debug builds;
void destructors retain their documented preconditions rather than leaking a
live object or pretending to clean external work.

Planned meaningful body tests, beyond existing M/core/agent/IPC suites:

- Manual-vs-run dispatch/service/save ordering and retained queue/rotation state;
  zero/default/max/overlimit step settings, exact event ceiling, continuous queued
  work, due service and finite idle with an actually idle writable IPC link.
- Stop before/inside/between units, separate threaded/signal-safe stop, handler/
  service/idle/save/clock failures, no invented clock on failure/backward/overflow;
  first-error latch, info.done, reentry from each callback, end exactly once and
  invalid-call output/state preservation. Manual step calls no wait/idle hook.
- Exact newly accepted and attached handshake→proof capture→frame→EOF/TX failure
  with saturated IPC event slots and arena; retained immutable handler proof,
  CONNECTED ordering, disjoint peers and untrusted enqueue rejection. Repeat
  after stop/end/rebegin, retain one existing connection's proof without duplicate
  CONNECTED, and remove old connection/cache before a new accepted link.
- Popped handler-error IPC/intent lease release, untouched queued intents across
  budget/stop/error, exact injected FIFO restoration, disposal releases leases
  without running queued effects. Pending reservations cannot be stolen meanwhile.
- Borrow pointer/ID/parent-link identity, null/unknown/remote/terminal/cancelled/
  duplicate refusal with unchanged output; return wrong record/pointer/no borrow/
  attached child refusal; child core/free before return; terminal ACK/intent-lease
  release still cannot free a borrowed record; return has no execution effect.

Independent private-layout review remains required before core.c/agent.c bodies.
The source stage will use strict GCC/Clang, separate ASan/UBSan/LSan and meaningful
TSan stop/concurrency checks; scoped passing tests cannot stand in for unfinished
O orchestrator/application/scenario integration or final P acceptance.

Header verification: strict standalone/repeated include checks passed6/6 — core
layout with GCC/Clang C11, agent layout with GCC/Clang C11 and G++/Clang++ C++11.
Production core.c and agent.c remain unchanged at this header-only checkpoint.
