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
