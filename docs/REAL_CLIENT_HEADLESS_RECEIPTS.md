# Private child-update receipt design

Design child `300e835` under N `035219b`, foundation `581d831`. This is a private
header/docs subset with no bodies, public amendment, runtime or acceptance claim.
The common control envelope, worker identity, application lifecycle and outer
session/role/fabric binding belong to the primary N architect. This payload adds
no separate transport or magic. Its declaration is
[control_receipt.h](../applications/cog-cli/src/control_receipt.h).

## Pass 1: goals, blocks and initial header

- Preserve contiguous parent CGA1 application and immutable accepted terminals.
- Retire a cancellation-rejected wire update without pretending it was accepted.
- Keep parent notification copy/local ACK separate from transport receipts.
- Bound message, receipt and tombstone state without replaying effects on loss.
- Refuse identity/capacity/crash ambiguity rather than synthesize authority.

The existing core owns semantic update status/sequence and retained notification.
The common envelope owner authenticates and retains exact wire identities. The
worker owns actual execution/drain and its immutable pending wire bytes. This
private scalar codec only exchanges an explicit parent disposition.

```mermaid
sequenceDiagram
    participant W as Worker/executor
    participant E as Parent common envelope owner
    participant C as Existing agent registry
    W->>E: immutable wire item w, CGA1 n
    E->>C: apply_update(n)
    C-->>E: accepted / AGAIN / cancel rejection
    E->>C: copy notification, queue owned copy, local ack (accepted only)
    E-->>W: typed receipt for w, parent semantic prefix k
    Note over W,E: CANCEL_CONSUMED retires w only; no semantic advance
    W->>W: actual cancel/drain; preserve factual tool ledger
    W->>E: new wire item w+1, admissible terminal k+1
```

The payload is fixed55 bytes: version1, explicit disposition, six big-endian
u64 identities/prefix fields, accepted-terminal status u8 and signed outcome
i32. There are no strings, secret bytes, native structures or borrowed payload
pointers. Parent runtime incarnation is distinct from durable owner epoch,
display/configuration generation and session identity. Actual owner_peer is a
transport peer, never a managed worker ID. The existing agent status/result types
are reused. Codecs only validate scalars and have transactional capacity errors.

| Disposition | Wire retirement | Semantic meaning |
|---|---|---|
| ACCEPTED | Exact matching item only | Core accepted that exact CGA1 sequence; notification was copied/queued and local ACK completed. |
| RETRY | None | Keep/retry immutable bytes. Core may already include that sequence while local notification delivery is blocked. Prefix alone never frees the item. |
| CANCEL_CONSUMED | Exact rejected item only | Parent admitted cancellation; RUNNING/COMPLETED n=k+1 was not accepted. After actual drain, a new wire item can fill the unaccepted slot with an admissible terminal. |
| TERMINAL_FIXED | Exact pending item only | Parent already accepted an immutable terminal; notification delivery/local ACK completed. Receipt does not claim the pending payload itself was accepted. |

The accepted-terminal field uses PENDING/OK only as an absence sentinel. Present
terminal predicates exactly preserve existing agent.h acceptance: COMPLETED/OK,
or FAILED/CANCELLED/TIMEOUT with a known negative result. RUNNING is not terminal.
The payload does not narrow accepted failure outcomes or redefine core states.

Capacity belongs to the enclosing runtime, not this pure codec. Before accepting
a wire item, the parent must reserve its disposition/receipt storage and the
worker must retain the exact item until a matching final disposition. Actual
drain and a final terminal need reserved capacity even when ordinary traffic is
full. Exhaustion returns backpressure/LIMIT at the owning boundary without
positive acknowledgement, discarded pending bytes, ID reuse or semantic skip.
No operation silently grows a receipt queue. Later passes trace exact loss,
cancellation, stale-wire and crash boundaries before implementation approval.

## Pass 2: cancellation, loss and crash traces

The existing authority is `cog_agent_apply_update` at agent.c424, exact-current
`cog_agent_ack_update` at484, admitted nonterminal `cog_agent_cancel` at183 and
terminal/ACK/lease-gated `cog_agent_release` at497. No codec function invokes them.
The outer common envelope establishes every identity before any of these traces.

| Trace | Sequence and required disposition |
|---|---|
| 1. Rejected RUNNING or COMPLETED | Parent prefix k0, wire10 carries RUNNING1, then parent admits cancel. Core rejects1 unchanged. No receipt causes endless retry; ordinary ACK then CANCELLED2 also deadlocks because parent expects1. Parent sends CANCEL_CONSUMED(wire10,k0). Worker retires wire10, actually drains, emits new wire11/CANCELLED1. Parent applies1, independently copies/queues notification, local-ACKs1, then ACCEPTED(wire11,k1,terminal CANCELLED). The same rule applies to an unaccepted COMPLETED n=k+1. |
| 2. Pending parent notification | Parent accepted RUNNING1 but its notification is pending. After cancel, COMPLETED2 is rejected. CANCEL_CONSUMED reports k1; drained CANCELLED2 may get AGAIN until the parent owns a copied/queued notification1 and local-ACKs1. Worker retries CANCELLED2 unchanged. It must not skip3 or fabricate ack2. If ingress itself succeeded before notification delivery blocked, RETRY may report the new accepted prefix; it still retains the wire bytes. |
| 3. Terminal arbitration | Parent accepts COMPLETED2 before cancel: cancel returns INVALID_STATE and terminal2 wins. Exact accepted retry gets ACCEPTED after the copy/ACK gate; a different pending update gets TERMINAL_FIXED after that same gate. No CANCELLED3. Worker-only physical completion is different: if the parent had not accepted it before cancel, the worker preserves factual child ledger/result, confirms drain, and reports cancellation in the still-unaccepted slot without claiming effects undone. |
| 4. Lost acceptance / duplicate ACK | Parent accepted n, but ACCEPTED was lost. Identical wire retry is idempotent: core's duplicate check precedes cancel/terminal rejection, including an accepted RUNNINGn retried after cancel. Re-send the retained immutable receipt. Local ack_update is only for current n, never an old wire ACK after advance. Worker matches the exact pending wire; an older ACK cannot free n+1, regress its known prefix or undo cancel. RETRY carrying accepted-terminal metadata still does not release storage before notification delivery/local ACK. |
| 5. Lost negative receipt / late old wire | CANCEL_CONSUMED lost: worker retries exact rejected bytes; parent repeats the retained rejection, with no semantic movement. Once new wire11/CANCELLED1 is accepted, delayed wire10/RUNNING1 must be handled by its retired-wire identity before core ingress, not submitted as a conflicting same-sequence terminal duplicate. Exact accepted-terminal retry is OK; different bytes are PROTOCOL and later semantic sequences INVALID_STATE. Duplicate cancellation never extends drain time. |
| 6. Worker crash | Parent uses its own accepted k, not worker's last sent n. Only after adequate attempt/descendant fencing/drain may it synthesize FAILED/TIMEOUT k+1, still respecting any pending notification k. Reaping a worker PID alone does not prove all descendants/effects stopped: uncertain durable intent remains recovery-only. An already accepted terminal is never replaced by death. |
| 7. Parent crash / reconnect | Disconnect is not a receipt. Worker neither discards pending bytes nor resets sequence or starts another attempt. Same live parent/attempt may reconnect with authenticated monotonic receipt state. A restarted parent has a new runtime incarnation; old receipt claims cannot restore volatile registry/accepted-prefix authority. Without an explicit durable correlated restoration path, old execution stays fenced/uncertain and uses existing recovery, not worker claims, k0 reset or effect replay. |

Only CANCEL_CONSUMED retires a **rejected** update and permits the new wire to
fill its unaccepted semantic slot. Acknowledging transport consumption is not
calling core apply or local ACK. Terminal metadata is only the parent's accepted
core status/outcome; it does not assert durable parent TOOL-result publication.
The child keeps factual effect/ledger evidence even when the parent result later
fails to save. Local accepted terminals must not be rewritten to fabricate the
parent's cancellation projection.

An authenticated CANCEL_CONSUMED can arrive before the common CANCEL command.
It starts actual drain immediately under the worker's already bounded local
drain policy. The common CANCEL carries the parent's absolute deadline/prefix;
when it arrives, combine by the earlier deadline, never extend an existing one.
Receipts add no independent deadline field, clock or wait. Duplicates do not
restart a timer. Drain completion remains a runtime fact, not a codec result.

All identity checks precede retirement: exact fabric, parent role/runtime,
session, admitted worker placement, actual owner peer, agent/attempt and pending
wire. Reject wrong peers/epochs, conflicting reused wire IDs, unverified higher
prefixes and malformed data. Neither stale traffic nor a receipt prefix alone
can authorize slot retirement, new effects, semantic skipping or recovery.
