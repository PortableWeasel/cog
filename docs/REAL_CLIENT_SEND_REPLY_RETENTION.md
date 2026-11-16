# Generic reply delivery retention

Ticket81431a3 supplements approved92a7d46. Header/docs only; source use gated.

## Pass 1: independent delivery owner

A generic incoming lane may retain a semantic request pending resolution while
also owning a provisional/final response. copied_wire/next_send_ms describe when
one retained wire may be copied again, but do not say whether a duplicate has
re-armed a response. pending_valid must keep its semantic request meaning where
used that way. PROFILE already has its independent dedicated pending flag and
needs no added field.

Add only generic lane pending_reply_wire:0 means no response waiting for delivery;
a nonzero value selects the exact existing final_reply_wire or provisional_wire
and immutable buffer. Arm it after constructing a valid response, or after a
validated duplicate requests its retained response. A deferred send or AGAIN
preserves selector, wire, bytes and existing send eligibility. Successful copied
send clears only the matching selector; it neither consumes semantic request nor
retires receipt proof. Later validated duplicate may re-arm the same wire while
preserving its copied_wire/next_send_ms cooldown.

A fair producer pass services an armed reply when eligible, even if no further
request arrives. When ineligible, its next_send_ms is included in the owner step's
next_due bound. Different streams keep independent selectors. This introduces no
buffer, ACK, new frame identity, wire kind, hot allocation or extra native read.

## Pass 2: pressure and replacement traces

1. First final response R gets AGAIN. The immutable final cache and selector R
   survive. Later service sends R even without another request. Copied OK clears
   selector only; final cache remains available until its normal consumption.
2. R was copied at time10; exact duplicate request arrives at time11. It re-arms
   selector R, but cannot reset the eligibility time1010. No send before1010;
   service at1010 retries exact R and updates only send scheduling on accepted
   copy. Repeated duplicates at time11 cannot create a tight response loop.
3. Provisional P for request W is pending, then the SAME request obtains a final
   response R. Final classification supersedes P's delivery: selector becomes R,
   and R has its own new wire, immediately eligible. No P is sent afterward; the
   semantic request and final receipt follow their normal acceptance rules.
4. A valid higher SAME-stream request W2 has been admitted and therefore proves
   W1's final response consumed under existing protocol. Its response replaces
   any older pending delivery. Conservative retained old cache bytes do not
   require an obsolete W1 reply to preempt W2's response. A delayed old request
   follows existing covered/stale rules. Merely parsing a malformed, unauthorized
   or unadmitted W2 cannot clear W1's selector or receipt ownership.
5. A duplicate of current active W re-arms only its current response (final if
   one exists). A stale provisional duplicate cannot replace an already final
   response selector. Invalid correlation never selects another buffer.
6. Response delivery and semantic pending can coexist: UPDATE RETRY leaves W
   pending after P copied OK, but selector becomes0 until an actual retry/demand
   arms it again. A new terminal UPDATE may obtain its own response normally;
   no generic response is sent proactively solely because final_valid is true.
7. On same-key physical reuse, retained receipt and any still-required delivery
   selector survive. Factual stream/native retirement clears selector with its
   associated proof. The new logical UI lease reset remains its own separate
   rule; no native key rebinding or semantic pending reset is implied.

## Pass 3: bounded lifetime audit

The selector is one uint64 in existing constructor-owned generic metadata, not a
second queue. A stream has at most one currently owed response: final dominates
provisional for the same request, and only a valid admitted higher successor may
retire the older response obligation. Existing byte caches remain the sole source
for exact reply encoding; no generic selector ever names credential-bearing data.
If neither retained response matches a nonzero selector, fail closed; never send
an unrelated or empty buffer. Reserve response bytes/identity before arming.

At fixed maxima this adds840*8=6720 scalar bytes per coordinator and360*8=2880 per
worker to92a7d46's metadata payload, for total20576 and8944 added scalar bytes
respectively; actual sizeof accounts for ABI layout. Existing calloc counts,
frame capacities and all hot-path allocation prohibitions remain unchanged.
There is no new counter to wrap; the selector is an existing validated nonzero
outer wire. No native reader, retry timer, semantic prefix, guard, resolver or
operation limit is added.

Required regression: firstreply AGAIN then service without duplicate; duplicate
inside cooldown then exactly-due send; zero/equal/backward clock eligibility;
provisional-to-final replacement with immutable final receipt; malformed higher
request cannot erase prior pending reply; original deadline unchanged; retained
same-key receipt survives reuse; actual key retirement clears delivery. Existing
native cancel5s and handoff15s oracles remain unmodified. Independent design gate
is required before source uses the new selector.
