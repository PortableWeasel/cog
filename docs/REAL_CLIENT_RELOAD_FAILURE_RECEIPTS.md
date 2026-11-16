# Failed resolved reload receipt retirement

Ticket 4a38e9f. Narrow purpose4 PROFILE clarification; existing CGB1, BOOTSTRAP_ACK,
private storage and public APIs are unchanged. This is a header-only amendment.

## Pass 1: consuming candidate and request-proof lifetime

The coordinator resolves one explicit reload and pins that exact candidate until
the correlated final BOOTSTRAP_ACK. The worker validates its original envelope,
probe and generation once. `execution_reload_resolved` consumes its candidate on
every return. Before invoking it, the worker detaches any candidate duplicate-proof
borrow from the exchange; the incoming candidate is locally owned until transfer.
No callback or concurrent ingress may observe an exchange pointing to a candidate
which that call can free. Preserve only original nonsecret correlation/probe values
across the call. This does not retire a successful request preemptively.

On success, publish duplicate proof from the exact admitted generation now owned
by the executor, with the original probe; ordinary success replay rules continue.
On failure, before returning ingress advance the PROFILE retired watermark through
this request, clear its source/probe duplicate proof, and wipe full secret scratch
and frame reservations. Encode only the actual failure, attempted generation and
state1 into the existing dedicated nonsecret BOOTSTRAP_ACK body. Preserve its
original session/owner/worker/agent/attempt, reply_to and allocated response wire.
Do not clone the failed candidate, cache secret original bytes, reread credentials,
re-resolve, retry execution_reload_resolved, or substitute the current generation.
The active execution/generation remains the actual outcome of the failed reload.

The exception is request-proof retirement, not negative-receipt consumption:
retired requests and a pending or copied negative ACK may coexist. The bounded
exchange already has independent retired_through, wire/replied_wire, body and
pending metadata. No added layout or allocation is necessary. Generic receipt
arenas never receive credential-bearing bytes.

## Pass 2: duplicate, pressure and loss traces

1. Candidate G2 fails once. Its secret is consumed/wiped, request wire W is
   retired, and negative ACK R(reply_to=W) remains in dedicated body. Copied-send
   AGAIN keeps that body and pending flag unchanged. No ingress path frees or
   overwrites it, and the current primary remains bound and serviceable.
2. An authenticated, well-framed request in the same full PROFILE stream at W
   arrives while R is pending or retained after copied-send. Classify it stale
   before candidate decoding or source access. It may re-arm the identical R;
   never allocate a new response wire, compare erased original bytes, or invoke
   reload again. Different payload bytes at retired W receive the same stale
   treatment: replaying R reports the historical failure of W, not byte equality
   or acceptance of the changed payload. Framing/authentication violations still
   retain their existing fatal/error policy; stale identity alone is nonfatal.
3. A delayed lower covered wire is uniformly stale with no guessed reply. Only
   the exact retained failed request key and wire can re-arm R. A different
   worker incarnation/session/owner/attempt is not that exchange. Duplicate
   traffic cannot reset deadlines, reopen resolution, or extend native lifetime.
4. R copied-send OK clears pending delivery, but retains R and correlation until
   an admitted higher request in the SAME PROFILE stream proves consumption, or
   explicit exchange/native lifetime retirement. A higher malformed, unauthorized
   or semantically rejected request does not prove consumption. A higher request
   cannot replace a still-pending R. No generic higher lane, publication, command
   ACK or different slot consumes this receipt. Response-wire counters never reset.
5. If R is lost before delivery, retry W re-arms R from nonsecret storage. If W
   and R are both delayed, bounded native operation deadlines still apply. If
   the binding is lost, no response or new owner is fabricated: preserve the
   caller's unresolved/frozen outcome and retire at actual lifetime end. A new
   binding cannot resurrect this consumed candidate or replay an old effect.
