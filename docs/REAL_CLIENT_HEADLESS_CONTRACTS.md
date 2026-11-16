# Headless and subagent application integration

Design `035219b` (N), delivery `0e35c23` (O); baseline `dacbd199`, after the
[M gate](REAL_CLIENT_M_GATE.md). This is a design worktree: no implementation
bodies or runtime acceptance are supplied. Existing E wire/ownership contracts
and shipped single-process guarantees remain authoritative. Three full header
passes and different final review gate every dependent O implementation.

## Goals and release boundary

1. Run one durable coding session without a connected UI, attach/reconnect a
   bounded local client and resynchronize active/incomplete display without
   executing model/tool work to reconstruct it.
2. Execute actual bounded local/owned-worker child prompts and tools, return
   correlated retained results, and cancel/drain/reap without replaying effects.
3. Bind every worker connection to an immutable native child identity and every
   command/update to its actual fabric, current placement and attempt identity.
4. Inherit an immutable effective configuration and an intersected permission/
   limit profile; never send secrets through argv, environment, UI replay or logs.
5. Preserve K/L/R12/observability/output/empty-session semantics while multiple
   application contexts make bounded cooperative progress.
6. Implement the declared quiescent same-host shared-store handoff with exact
   checkpoint/CAS/guard/route correlation. Reject live/cross-host transfer and
   automatic resurrection of a crashed uncertain attempt.
7. Replace original03/07/09/10/11/17 synthetic/disconnected compositions with
   actual joined execution. Restore all six explicit M exclusions and retain
   every existing assertion. P still requires all38/full-suite acceptance.

## Blocks and responsibility cross-check

| Block | Sole authority / reused primitive | Goals |
|---|---|---|
| Public IPC identity/directed send | OS peer credentials plus existing nonblocking endpoint/framing/peer lifetime | 1,3 |
| Cooperative core/app drive | Existing Q fair dispatch, event/agent leases and deadlines; no nested run | 2,5 |
| Headless front door | One bounded control core/UI listener, command receipts and client roles; no execution lease | 1,3 |
| Executing application | Existing K state machine, durable session guard, replay owner and atomic display anchor | 1,2,5 |
| Child registry binding | Existing copied agent intents/CGA1 retained update/ack, explicit execution-session borrow | 2,3 |
| Orchestrator | Separate worker-control listener, immutable fork PIDs, bounded handshake/health/shutdown/reap | 2,3 |
| Configuration/permissions | J-owned resolved clone and fixed child profile, no retained resolver authority | 4,5 |
| Handoff coordinator | Existing G exact checkpoint/epoch/guard and CGH1 correlation; staged app lifecycle | 6 |

The UI and worker-control fabrics have different owners and socket paths. A
single endpoint has one receive/service owner. Q's existing control-core IPC
ingress is reused for UI delivery, not consumed simultaneously by orchestrator.
All peer references include their fabric; equal numeric IDs in distinct fabrics
are unrelated. The front door outlives a replaceable local/remote executor during
handoff and never derives an execution lease from a route or socket.

## Shipped inventory and verified remaining gaps

Existing primitives are complete and are consumed, not reimplemented:

- Q: nonblocking endpoint service, bounded complete-frame retention, fair core
  ingress/dispatch and leased event payloads.
- Local agents: copied spawn config/child session identity; immutable attempt
  owner/epoch; spawn/cancel leases; apply/copy/ack/release and CGA1 update codec.
- Replay: bounded copied DATA/GAP/CURSOR ring and CGR1 codec. CGD1 snapshots
  already include active/incomplete text with transactional decoding.
- G/L: exact immutable checkpoints, durable owner CAS and normal/recovery guards;
  explicit per-call reconciliation and no replay of uncertain effects.
- R12: two-owner whole-turn generation pinning and checked publication/retirement.
- Observation/output: copied bounded metadata, separate lost-feed error, consuming
  cleanup, bounded host output and checked meaningful authorization prompts.
- Empty sessions: initial checkpoint before app publication and guarded repair of
  owner-only reservations, with recognized legacy remnants refused.

The current orchestrator still implements the obsolete synchronous text loop.
Seven declared operations have no bodies: service, request_shutdown, publish_owner,
session_owner, forget_owner and handoff_encode/decode. Existing spawn/stop/free
also wait, peer/session storage grows during service, and no endpoint_service
drives handshakes. `d5280b7` owns that real migration, not fixture-only acceptance
of AGAIN. M's exact remaining failures are unit-orchestrator, integration-
orchestrator-ipc/stress and original07/09/10.

The app currently has one execution context; --socket only transfers a listener
to core. It has no actual IPC/agent-intent dispatch, headless client, worker host,
child executor or replay join. Original03 manually fabricates child updates;
07 separates process crash from agent failure; 09/10 separate text routing and
fabricated task completion; 11 uses a test-local output buffer; 17 labels plain
save/load as migration. These are component/history assertions to preserve,
not end-to-end acceptance for O.

## Required amendment register (first-pass inventory)

| ID | Concrete contract gap | Proposed minimal boundary |
|---|---|---|
| N01 | peer_id is listener-scoped routing, not authentication; HELLO currently mutates owned PID | public IPC peer-credential query; immutable native PID from spawn; bind actual owned PID/UID before worker readiness |
| N02 | nonblocking spawn reserves identity before connection order is known | distinct managed child identity and authenticated transport mapping, never silently redefine IPC peer_id |
| N03 | core_run/app_run block; nested local child loops violate fairness | bounded nonrecursive core/app step lifecycle sharing existing dispatch/service/lease semantics |
| N04 | registry owns child session but app_new always creates another | explicit session execution borrow/return and child constructor binding exact identity/parentage/guard |
| N05 | CGA1 encodes updates, not spawn/cancel/config/permission requests or receipts | bounded role-checked control envelope, immutable accepted command IDs, retained receipts/dedup, cancellation receipt sequencing |
| N06 | private display_sequence and replay append sequence are independent | one app-owned canonical commit bridge; CGR1 DATA carries CGD1 replacement state at the exact same anchor |
| N07 | GAP snapshot omits current interactive permission control | targeted atomic policy state/nonce after resync, bound to exact node/call; no stale/prequeued approval |
| N08 | RESUME constructor acquires/advances authority immediately | explicit staged exact-checkpoint application preparation/activation lifecycle for CGH1, no nested app_new workaround |
| N09 | child resolver/secret/config authority not defined | constructor-owned resolved clone; bounded authenticated private bootstrap; permission/limit intersection and scrubbing |
| N10 | current fixed built-ins have no actual model delegation executor | static application tool-table composition plus explicit delegate task-only schema, identified durable parent intent and child result |

### Protected invariants and compound cases

- Native PIDs used for signals/reap come only from successful owned spawn and
  never change on wire HELLO, task/model content or session routing. Impostor
  connection order, forged PID and PID reuse after reap must fail closed.
- A child is not ready merely because connect/send returned OK; UNKNOWN health
  admits no task. AGAIN accepts no new command; OK retains exactly one copy.
- Parent cancellation racing an in-flight update needs an explicit receipt
  containing the last parent-accepted sequence. An already accepted terminal
  wins. Otherwise the worker drains and supplies one admissible terminal without
  deadlocking contiguous CGA1 sequencing or claiming effects were undone.
- UI reconnect never retransmits an accepted effect-bearing command as a new
  command. Session/generation/controller/request identities and bounded retained
  receipts are separate from display cursors and durable owner epochs.
- An old queued SUBMIT/ALLOW cannot become a fresh incomplete-turn continuation
  or authorize a different call after delayed dispatch. Continuation/permission
  require explicit current-state correlation; K/L's fresh-input rule remains.
- A failed display/replay commit freezes admission and reports explicit error;
  it never returns a successful partial snapshot or silently drops accepted
  output. Snapshot/cursor transfer owns bounded copied storage through send.
- Child configuration is pinned at actual spawn admission and outlives that
  child's provider disconnect. Parent reload cannot relabel or elevate it. Child
  permissions/limits only narrow; no callback authority is retained or invented.
- Local child execution borrows the registry's exact session rather than
  constructing an unrelated tree with the same string ID. Release/free refuses
  live execution borrows and unacknowledged results.
- Forced process death is not proof that descendants' side effects stopped.
  Affected durable intent stays uncertain, no automatic task/tool replay or
  failover is allowed; explicit fencing/recovery remains required.
- Handoff is only quiescent complete replay-safe state with no live child/handle
  or unresolved result. Target PREPARED has no execution authority. Guarded READY
  and exact route publication precede activation; post-CAS failure never restores
  the old epoch. Uncertain fsync remains frozen, not guessed rollback.

## Next design checkpoints

Pass1 will declare the precise public/private interfaces and fixed constructor
bounds, with a standalone block diagram. Pass2 retraces all20 originals plus
R13/R14 and the compound cases above, amending every actual call gap. Pass3
checks errors/overflow/identities/ownership/dependency cycles and validates public
headers with the maintained critic-header-smoke target under GCC and Clang.
Different final review must pass before any O body. Gated per-file source,
review, integration and finalize children will name exact ownership groups and
actual process/TLS/tool acceptance oracles.

## Incremental pass1 foundation subset

The first header subset declares two IPC operations: exact directed send (no
temporary/fake session route) and copied kernel peer identity. Neither makes a
link ID an execution credential. Higher layers still match unreaped immutable
native spawn ownership and an exact managed bootstrap incarnation, including
stale buffered HELLO after reap/PID reuse.

Core driving has explicit begin/step/end. The SAME checked monotonic-ms clock,
dispatch/IPC engine, fair queue rotation, interval/deadline counters and lease
cleanup back run and manual driving. A step has a constructor dispatch ceiling,
never calls idle/wait hooks and reports its count/deadline. It cannot nest into
itself or run, and a budget yield is not an exit. End consumes only stopped/failed
driver ownership and returns its retained primary error; asynchronous work still
belongs to the host. This avoids independent synthetic clocks and a second event
loop implementation. Application stepping will add its existing drain/policy/
input ownership rather than recursively call app_run.

The local agent borrow/return pair exposes the exact registry-owned session with
one execution lease. Remote records cannot borrow their local identity shadow
for execution. App child construction borrows instead of freeing/recreating this
tree; parent release refuses a live borrow. Return follows all child app/core,
provider/config, tool/process and store-guard cleanup. Parent core destruction
retains its explicit drained-work/no-borrow precondition; no destructor leak
workaround or unbounded join is introduced.

This subset is for incremental critique, not completion of pass1 or the N gate.

Incremental critique requires immediate-work readiness, not just timer deadlines.
The step result includes a finite max_wait_ms, zero for actionable queued work,
and a due deadline on a budget/fairness yield. The host takes the minimum across
all contexts/its own readiness; UINT64_MAX is never an unbounded sleep authority.
Opaque service hooks retain their existing duty to report due work. A connection's
cached immutable authentication/role proof must also outlive already-owned queued
frame event leases: a final accepted frame can dispatch after Q observes EOF and
closes the socket. A fresh credential query at every message handler is not a
valid replacement for that retained ingress proof. New unadmitted connections
may still be refused fail-closed; no old peer ID binds a replacement connection.
Core require_peer_credentials now captures a by-value identity in each protected
IPC event before the endpoint can disappear. Generic core_enqueue rejects a
claimed proof; otherwise a public producer could forge OS provenance. Generic
non-authenticated IPC remains available with proof=false. The headless handler
requires protected ingress plus its own retained authenticated role/attempt
binding, and validates the event's semantic kind before its payload.

## Incremental managed worker identity

Nonblocking spawn returns strong `cog_worker_id_t{incarnation,number}` before
there is a connected socket. Stop/kill/health use that managed identity so they
work before authentication. `core_info` exposes BOTH it and the actual transport
peer (zero until ready), plus immutable owned PID and explicit reap status.
Placement, CGA1 owner_peer, CGH1 source/target and owner routes keep actual peer
identities. No cast or connect-order coincidence bridges these namespaces.

Kernel PID/UID must match the still-unreaped spawn and the worker must echo its
exact trusted-startup managed identity before readiness. HELLO contains no PID
authority. Counter exhaustion fails before fork; old/foreign IDs cannot select a
new worker. An already reaped record never signals its old PID, even if reused.
No socket disconnect alone proves descendants/effects stopped. Management/app
message ownership and bounded event delivery follow in the next subset; these
signature changes deliberately require O consumer/fixture migration.

## Incremental orchestrator delivery bridge

Orchestrator alone receives/services worker sockets. Copied application events
and exact send-to-managed-worker provide the app bridge; no second endpoint
reader is exposed. Normal message events use constructor-count/byte reservations.
Three per-worker control transitions are independently reserved, so full ordinary
storage cannot prevent timers, native shutdown and reap. Wire FIFO pressure may
delay heartbeat/ACK frames; no contract pretends it can skip a retained large
frame. Native TERM/KILL deadlines remain the cancellation escape, with all
affected attempts frozen and descendant/effect uncertainty preserved.

The supported host keeps SIGCHLD neither ignored nor SA_NOCLDWAIT, unchanged
while managed children exist, and grants exclusive exact-PID reap ownership.
Constructor/spawn validate disposition without installing a handler. Every
signal first checks waitpid(exactPID,WNOHANG); ECHILD permanently loses ownership,
forbids signalling/rebinding and reports IO. With the honored lifetime condition,
exit between check/signal leaves an unreaped zombie, preventing PID reuse. This
is an explicit profile, not magical protection against a violating foreign
thread. No waitpid(-1) consumes unrelated host children. Tests cover unsupported
disposition, external-reap/ECHILD refusal, unrelated child and retired ID reuse.

Reaped records remain bounded: kill is idempotent only while the exact reaped
record is retained; release_worker requires no remaining events/routes. Retired
IDs are NOT_FOUND and never select a recycled slot. Fresh constructor incarnation
has checked entropy/setup failure, no PID/time fallback. A managed worker binds
one actual transport; loss ends that attempt with retained/frozen uncertainty,
not worker reattachment under a fresh peer ID. Receipt/header subset300e835 is
independently reviewed before N-only integration; it does not add rebind authority.
