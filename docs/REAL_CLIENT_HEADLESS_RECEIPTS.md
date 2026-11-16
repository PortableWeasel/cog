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
