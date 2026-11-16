# Retained control sends and transport ownership

Ticket92a7d46; concrete source failures fcda02c and7e3eb45. Existing CGL/Q schemas,
semantic receipts and native bindings remain unchanged. Header-only amendment.

## Pass 1: explicit bounded ownership state

A copied-send OK means the exact authenticated native endpoint owns one copy for
ordered nonblocking delivery. It does not mean execution, semantic acceptance,
receipt consumption, cleanup, or successful handoff. Sending the same held frame
on every outer slice creates distinct queued copies; bounded queues do not make
that fair to later control records.

Each retained outbound generic stream and dedicated PROFILE exchange owns two
constructor scalars: copied_wire and next_send_ms. copied_wire identifies the
actual outer wire accepted by copied send, and next_send_ms is earliest permitted
retry on the SAME authenticated binding. No frame bytes, proof, receipt, key or
counter are retired by these scalars. Their storage is included in the existing
bounded constructor metadata arrays; no new buffer, heap call or hot allocation.
The existing lane deadline_ms remains min-only operation ownership and is never
repurposed. Use CLI_CONTROL_RETRY_MS=1000 milliseconds for request/reply retries.

A new retained wire unequal to copied_wire is immediately eligible. Before each
send, owner/native/role authorization checks still apply. AGAIN accepts nothing:
retain exact bytes and eligibility, then retry on later fair service slices. OK
sets copied_wire to that outer wire and next_send_ms to checked now+1000; inability
to represent the future time is LIMIT before accepting a new copy. Subsequent
request/reply sends of that wire are ineligible until that time. Repeated valid
caller requests, duplicate responses, zero max-wait loops and PROFILE callbacks
never reset eligibility or allocate a fresh wire. Original finite operation and
native deadlines stay unchanged and still force failure/cleanup when reached.

One-way CHILD_CANCEL and final UPDATE cleanup confirmation need no response to
prove transport admission: after copied-send OK do not enqueue more copies on
that binding. This never proves cancellation completed or terminal cleanup was
received; their existing factual terminal/receipt/native-lifetime rules remain.
Keep whatever immutable frame/proof those rules require; no fake acknowledgement.

HANDOFF uses the addressed slot outbound HANDOFF lane's send state, not a new
transaction-wide timer. The source and target keep separate stream metadata.
PROFILE uses dedicated exchange metadata and arenas only. A delegation handle's
held request uses its executor outbound lane; no duplicate scheduler-owned buffer.
Native CGW heartbeat/shutdown and UI client reconnect/sent flags retain their own
existing ownership; this amendment neither changes those protocols nor permits
cross-binding replay. Bodies remain gated on independent review.
