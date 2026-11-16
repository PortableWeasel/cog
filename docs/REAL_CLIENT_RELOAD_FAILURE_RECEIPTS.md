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
