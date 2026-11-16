# Terminal UPDATE receipt retirement (4536a17)

## Pass 1 — exact existing-kind confirmation

An ordinary terminal worker attempt has no required later semantic update, so
parent TOOL persistence alone cannot establish receipt consumption. Add no kind,
codec field, receipt payload, semantic sequence or transport API. The one narrow
outer mapping is COMMAND_ACK with UPDATE lane, empty payload, from the exact
admitted authenticated worker to its coordinator. This does not alter ordinary
COMMAND/CANCEL command acknowledgement behavior.

Worker emits this only after validating/consuming terminal ACCEPTED or
TERMINAL_FIXED and finishing actual child executor/handle/guard/provider/config/
net/observer cleanup. RETRY, RUNNING ACCEPTED and CANCEL_CONSUMED cannot trigger
it. The confirmation has fresh increasing worker UPDATE wire and reply_to equal
to the exact final UPDATE_RECEIPT OUTER wire. That receipt payload's wire_id names
the worker update; it is deliberately a different correlation. Runtime, managed
worker, immutable kernel/native peer, owner_peer, session, agent and attempt must
all match the retained final disposition and original immutable worker envelope.

Coordinator checks that proof before retiring the final cached UPDATE original/
receipt into scalar watermarks. It does not apply_update, copy_update, local
ack_update, alter the accepted terminal, release the parent guard or invent a
TOOL checkpoint. Child slot/registry/group reuse still additionally requires all
actual cleanup, execution/intent lease return and durable parent TOOL-result ACK
conditions. Whichever proof arrives first is retained until the others arrive.
No command result or ACK-of-ACK is produced for this consumption confirmation.
