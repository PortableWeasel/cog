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
sets copied_wire to that outer wire. For request/reply frames it also sets
next_send_ms to checked now+1000; inability
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

## Pass 2: traces across pressure, receipts and lifetimes

1. First CHILD_CANCEL W gets AGAIN: copied_wire remains different, so the exact W
   remains eligible and cancellation is not claimed delivered. First OK transfers
   one copy; retain cancellation latch/deadline and any required original, but no
   further W copies. Worker factual terminal and coordinator UPDATE receipt still
   proceed independently. Repeated cancel calls do not manufacture new W.
2. HANDOFF COMMIT W gets OK, but READY is delayed: preserve W and the entire phase
   tuple. Ordinary service cannot resend until next_send_ms. At that time one
   exact W may be copied and the retry eligibility moves by another1000ms; this
   does not move the original whole-transaction deadline. A lost READY may thus
   recover from the target's immutable receipt; CAS/acquire never repeat as new
   operations. ACTIVATE gets its own higher wire and is immediately eligible.
3. Copied request W remains queued in transport while its interval elapses: a
   bounded retry may add another exact W, at most one accepted copy per interval.
   AGAIN leaves the due retry eligible without filling additional queue cells.
   Service/receive/native shutdown continues while application send is deferred.
4. Worker UPDATE RETRY for W leaves semantic prefix unchanged and preserves the
   same request wire. It may mark existing work pending but does not bypass the
   send interval; ACCEPTED/CANCEL_CONSUMED/TERMINAL_FIXED retain their existing
   semantic rules. A new admissible terminal UPDATE uses its own new wire and is
   immediately eligible. One-way cleanup confirmation stops after copied OK.
5. PROFILE successful or failed receipt is delayed: retry only exact immutable
   request at the finite interval, never re-resolve or reconstruct from latest.
   The failed-reload negative ACK rule remains ticket4a38e9f: stale covered W may
   re-arm its existing nonsecret reply R, but cannot reset R's copied_wire or
   next_send_ms. Request proof retirement and send eligibility are independent.
6. Receipt R copied OK is not proactively resent after pending-delivery ownership
   clears. A matching duplicate request may re-arm R under the same eligibility
   window. Invalid payload/auth/key does not re-arm it. Full active exact lookup
   and existing stale-watermark rules precede any scheduling decision.
7. A legitimate new wire after valid same-stream successor proof is eligible
   immediately even when the previous wire's next_send_ms lies in the future.
   No scheduler grants admission to a successor that existing protocol rejects.
8. Send IO/protocol/native loss freezes or retires through existing driver rules;
   never clear the copied marker to retry effects on a replacement binding. UI
   same-lease reconnect preserves actual UI command receipts; an expired new UI
   lease resets only its own old state under existing rules. Native CGL has no
   transparent reconnect/rebind. Operation/native lifetime retirement clears send
   state together with its key; physical slot reuse cannot reset retained proof.
9. Abort/timeout keeps the original transaction deadline and unresolved receipt.
   A new valid cleanup wire is immediately eligible. Retrying an already copied
   cleanup wire still uses the separate next-send time, even when the original
   transaction deadline has expired; it must not clamp retry time to that past
   deadline and thereby recreate a busy resend loop. Native termination remains
   bounded by its existing deadline and supplies no fictitious cleanup receipt.

## Pass 3: consistency and exhaustion audit

| Boundary | Required result |
|---|---|
| Full stream identity | Compare/validate actual native binding and immutable current frame before scheduling. copied_wire alone is never authorization or a substitute for full key/byte correlation. |
| Fresh / AGAIN / copied | Zero or different copied_wire permits the retained current wire; AGAIN preserves ownership; OK records one transport copy. No pending semantic flag is cleared merely to throttle sends. |
| Exact retries | Same retained wire/body/probe and original operation deadline; no fresh nonce, operation attempt, CAS, provider call or resolution. A due retry is still subject to existing semantic/owner checks. |
| RETRY / duplicate reply | May preserve or re-arm existing pending work, but cannot move next_send_ms earlier. No tight echo/resend loop. |
| One-way copied frame | Stop submitting copies after OK; keep terminal facts/receipt storage until their separate real retirement predicates. No invented response or acknowledgement. |
| New valid successor | Fresh wire may send immediately; previous schedule is irrelevant only after ordinary successor admission. The same physical key/wire never gains new bytes. |
| Timeout / cancellation | Earliest operation/native deadline remains separately reported and enforced. Cleanup sends have eligibility clocks but never gain extra operation/native lifetime. |
| Poll/service fairness | While retained delivery is pending and ineligible, report next_send_ms as a wake-up bound alongside other earlier timers; continue native service, inbound receipts, other lanes and executor drain. No spin solely to resubmit an accepted copy. |
| Overflow | Check now+1000 before an accepted request/reply copy; LIMIT/frozen handling on overflow, no wrap or clamp to an already expired timestamp. One-way copies need no future retry timestamp. |
| Retirement/reuse | Clear send scalars only with factual stream/key retirement; preserve them when retained proof survives PROFILE or same-key physical reuse. Never rebind a previously copied request to a new process. |
| Failure receipt | Negative PROFILE R contains no credentials. Re-arming it preserves its existing outer wire and send schedule; consumed failed candidate proof is never reconstructed. |

Constructor accounting uses the actual enlarged sizeof of existing metadata.
At current fixed maxima the coordinator has42 keys*20 lane records plus26 secret
exchanges; a worker has18*20 lanes plus19 secret exchanges. Added scalar payload
is16 bytes per record, i.e.13856 bytes per coordinator and6064 per worker, before
any ABI padding already included by sizeof. Buffer counts/capacities, request
credits, frame ceilings and semantic limits do not change. No mutable globals or
implicit allocation/constructor is added. Static dispatch stays unchanged.

This amendment grants no native failover, transparent replay, deadline increase,
or semantic ACK shortcut. Required writer oracles: actual accepted-send counts
stay bounded under repeated zero-wait slices; AGAIN retries eventually copy;
retained reply loss triggers exact retry only when eligible; one-way cancellation
and terminal confirmation do not queue duplicate accepted copies; actual native
cancel completes within the existing5s cleanup oracle; actual moves complete
within the existing15s transaction deadline with exact CAS/READY/route/ACTIVATE
and retained disposal receipts. Full runtime acceptance remains separate.
