# Core runtime contract — Stage 14.Q

`include/cog/core.h` is authoritative. Q implements every core.h declaration;
public agent remote-update/replay/handoff integration belongs to N/O.

Construction checks products before allocating fixed storage. The external
event queue retains its configured capacity (default 64). IPC ingress has a
separate descriptor reserve derived from `ipc_service_quantum` (default 16),
so the constructor reserves **event_queue_capacity + ipc_service_quantum**
queued event descriptors, plus dispatch-local snapshots. This distinction is
necessary: a local handler can refill the external queue after every dispatch,
and servicing IPC alone would otherwise never obtain an ingress reservation.
Dispatch rotates between these sources, preserving each source's FIFO order.

IPC payload storage is the explicit `ipc_payload_bytes` arena (default 8 MiB),
with an admission ceiling `ipc_max_frame_bytes` (default 1 MiB). The arena must
fit at least one maximum frame. Payload slices retain ring wrap padding until
the corresponding FIFO dispatch releases it. An IPC descriptor and sufficient
contiguous arena bytes are reserved before consuming a retained endpoint frame.
The zero-capacity receive probe consumes only a zero-byte frame, for which the
already reserved descriptor suffices. Full reservations leave the endpoint's
same frame retained; servicing, other peers and local dispatch continue.
No per-message malloc/realloc occurs, and payload bytes remain valid until the
handler returns. A frame above the core ceiling closes its offending peer and
propagates LIMIT; the queued disconnect event remains for the next run.

The core reserves `ipc_peer_capacity` tracking pointers (default 16). Accepted
endpoints are explicit IPC constructors; excess connections are refused by a
bounded allocation-free accept/close operation. Each fairness step rotates
acceptance and peer service/receive attempts for at most `ipc_service_quantum`
units. Each endpoint service unit performs at most one bounded framing syscall
or publication. Readiness of a prefix never authorizes a blocking full read.
Normal EOF reports disconnect and does not fail other peers. Malformed frames
and resource/transport failures propagate according to the IPC contract.

The service hook runs before first dispatch, before idle, at due monotonic
deadlines, and at most `service_interval` dispatches apart (default 16). It runs
under a continuously nonempty external queue. OK and AGAIN are normal; other
service/idle results propagate exactly. Stop is checked around every service
and dispatch boundary. `max_idle_wait_ms` defaults to 10 and is reduced by the
next absolute deadline; a past/due deadline yields zero. Without an idle hook,
the platform sleep uses that same budget. Legacy wait_fn cannot be combined
with service_fn or idle_fn. Callbacks must remain bounded, as specified by the
public header. No provider/net/toolkit implementation dependency is introduced.

Pending message storage reserves owner slots, per-owner FIFO descriptors, and
per-entry text buffers from the configured limits (defaults 17/8/65536). Owner
IDs reserve 128 bytes including NUL. Long text/IDs or owner exhaustion return
LIMIT atomically; an individual full FIFO returns OVERFLOW. Empty owner slots
are reusable. Overflow dequeue reports the required length and retains the
message. Successful auto-enqueue copies into a constructor-reserved dispatch
buffer and logically removes the FIFO head immediately. An explicit owner lease
holds one restoration slot until dispatch, so public dequeue/count see only the
remaining logical FIFO, draining cannot change the injected item, and another
session cannot reuse its owner. Queue admission counts that leased physical slot
when checking reservations. Stop/error before dispatch removes the descriptor
and restores the exact copied item at the head ahead of replacements, without
allocation, loss or duplication. Dispatch releases the lease and never removes
a mutable FIFO head by a remembered index.

The core reserves agent records and five per-record string buffers using
max_agents/max_agent_message_bytes (defaults 16/65536). Local spawn uses the
explicit child-session constructor, copies task/parent IDs, and never grows the
registry. Per-agent requested status/result limits cannot exceed its reservation;
oversized replacement is atomic LIMIT. N/O still owns the new remote attempt,
notification acknowledgment, release/reuse, and wire-control APIs. No success
stub is supplied for these unimplemented protocols.

Session/provider/obs/store remain borrowed; the IPC endpoint transfers ownership
only on successful construction. Session attachment and existing auto-save
behavior remain unchanged. Constructor failure releases only new core storage.
Free releases owned endpoints, accepted clients, reservations and child sessions.
An active run cannot be reentered even after stop clears its public running flag.

Validation is recorded in `docs/REAL_CLIENT_Q_EVIDENCE.md`; Q passed independent
revision review and scoped main integration. Real-library integration with
separately migrated persistence remains check `b8a6fc1` before release gate P.
