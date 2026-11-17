# Handoff disposal receipts (2fee64f)

This clarification extends the approved5b90444 existing-kind phase mapping. No
new CGH/CGL kind, wire field, public API, allocation or private layout is added.
It is a resource-cleanup proof, distinct from durable authority transition.

## Pass 1 — successful disposal sequence

After exact target ACTIVATE succeeds and its correlated response is received,
coordinator sends a fresh CGL HANDOFF-lane request nesting CGH ABORT to the OLD
frozen source. It carries the admitted source/target/session/expected/next/token
unchanged. This request does not target the newly activated executor. Local old
source follows the same actual bounded drain/checked close, without wire proof.

Receiver validates original authenticated coordinator and native binding, full
stream/transaction tuple and phase before accepting the request. request_shutdown
only starts cleanup. No ABORT success response is sent while any executor/tool/
guard/provider/config/net/observer ownership remains. After actual successful
teardown, final CGH ABORT echoes the exact tuple and has outer reply_to equal to
that request wire. Coordinator validates the full binding/correlation and only
then permits reuse of that physical executor slot. Existing target authority is
unchanged; the receipt proves no rollback, effect reversal or target completion.

The existing slot HANDOFF pending/final buffers outlive app/net disposal. Before
freeing the app, retain the complete tuple/request bytes needed for response;
never read disposed app fields to answer retries. Receipt storage is not cleared
with executor resources, and source-release PREPARED proof cannot disappear
merely because execution_close consumed the source.

## Pass 2 — duplicate, loss and reuse traces

1. Exact ABORT retry while draining retains the same request and deadline; it
   neither closes twice nor emits a preliminary success echo. Changed bytes under
   that wire are conflicting duplicate; stale/mismatched tuple never disposes a
   newer slot occupant. Once final exists, exact retry replays cached bytes only.
2. Lost final ABORT: resources are already disposed but coordinator reserves the
   source slot until correlated response arrives. Receiver resends cached final
   without dereferencing app/net. Timeout preserves admission freeze and first
   failure; it does not authorize slot reuse based on socket send or process reap.
3. Source release PREPARED remains until fresh valid ABORT in the SAME HANDOFF
   stream. Coordinator only issues ABORT after receiving preceding final proof.
   Successful final ABORT then occupies that lane's bounded final cache. A later
   valid higher HANDOFF request in that same root/native stream consumes it into
   the retired watermark after coordinator actually received the disposal result.
4. Physical-resource reuse and proof retirement are distinct. Coordinator first
   receives successful final ABORT, then may send purpose3 PROFILE for a later
   same-root move. Worker may admit profile into actually freed resources while
   retaining old HANDOFF original/reply/key/counters. PROFILE cannot consume that
   proof; subsequent valid CHECKPOINTED in the SAME HANDOFF stream does so. Thus
   bootstrap-before-CHECKPOINTED ordering has no receipt-retirement deadlock and
   a lost old ABORT response can still be replayed without touching the new app.
5. An unrelated session/managed-peer key must not overwrite a retained final.
   Existing bounded capacity/physical-slot admission refuses work until valid
   retirement; no new cache, unbounded log or guessed conflict evidence is added.
   Native lifetime retirement invalidates its old bindings, not success proof.

## Pass 3 — cleanup failure and consistency audit

A consuming close error is still an error even if it consumed some handles. The
receiver retains factual ownership for unfinished cleanup and a correlated FAILED
result when terminal failure is known. It never emits successful ABORT for partial
teardown, silently frees a guard borrower or substitutes force/reap for cleanup.
First failure stays primary; the coordinator freezes further slot admission and
uses existing checked shutdown/recovery policy. No old epoch is reacquired and no
already activated target is relabelled as unactivated to hide a cleanup failure.

ABORT during failed/pre-CAS handoff uses exactly the same final post-cleanup
meaning for the addressed source or staged target. It is not successful PREPARED,
READY, ACTIVATE or CAS evidence. A target bootstrap reservation with no app must
still release its actual dedicated configuration/profile borrows and clear secret
storage before successful final disposal. A request to the wrong target or newer
transaction is refused before mutation. If no resources ever existed, authenticated
exact admitted transaction cleanup can finish without fabricating an app lifetime.

Whole handoff stays pending through old-source disposal and uses its original
finite transaction deadline; repeats never extend it or permit a second move.
Target authority and ordinary-work eligibility remain governed by the already
accepted ACTIVATE proof, never by source cleanup receipt. Send-AGAIN holds exact
bytes and retries one bounded send; receipt storage and cleanup IDs were reserved
at transaction admission. Existing CLI_HANDOFF_CLEANUP plus slot/lane state is
sufficient; no new phase value, callback, public header or constructor is required.

Consistency review: exact tuples, original native peer binding, distinct inbound/
outbound keys and reply_to are unchanged; proof survives app disposal and physical
same-root reuse; unrelated PROFILE cannot retire HANDOFF state. Final cache never
requires an infinite ACK-of-ACK chain: receiver retains it through reuse until the
next admitted SAME-stream request (or native lifetime retirement). Normal duplicate
lookup occurs before new-occupant dispatch, so old ABORT cannot kill new execution.
