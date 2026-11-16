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

## Pass 2 — lost, duplicate and reordered proof traces

1. Lost final receipt: worker still owns the exact original UPDATE and retries
   it; coordinator retains original bytes/final receipt even if parent TOOL ACK
   arrived first. Exact duplicate replays the same receipt. Worker cannot confirm
   an unreceived receipt or infer its outer wire from the semantic prefix.
2. RETRY then final: provisional receipt never permits confirmation, even when
   its prefix already includes applied terminal. Final validates notification
   copy/local ACK and terminal metadata before worker consumes its pending item.
3. Confirmation send pressure: retain one immutable empty ACK frame, its fresh
   wire and exact reply_to across AGAIN. Do not free/rebind that pending storage
   merely because app cleanup ended. Successful copied-send transfers the frame
   into the existing IPC-owned TX lifetime; sender may release its local copy.
   No synchronous wait, heap growth, second reader or recursive ACK is added.
4. The live native Unix stream is ordered/reliable; accepted TX owns delivery
   until service succeeds or transport fails. A broken link before confirmation
   reaches coordinator is NOT retirement proof. Coordinator keeps final cache
   and freezes/drains the attempt under existing native-loss policy. Neither side
   reconnects/rebinds that attempt. Process/peer loss remains factual uncertainty,
   never fake receipt consumption. The one-way exchange assumes no application
   drops a successfully received valid frame: receive-copy must retain it until
   this bounded owner-thread handler processes it.
5. Duplicate confirmation while proof exists is checked against exact retained
   receipt correlation and bound identity; it causes no second semantic ACK or
   cleanup. After watermark retirement, covered old UPDATE/confirmation IDs are
   uniformly stale-rejected with no response or guessed byte-conflict claim.
   No reply is sent even for a duplicate confirmation, preventing ACK loops.
6. If confirmation arrives before parent TOOL save, mark wire consumption but
   retain group/member/registry ownership until durable parent result ACK and
   all actual lease/cleanup conditions. If TOOL ACK arrives first, keep the
   original UPDATE/final receipt intact until confirmation. Neither ordering
   rewrites the immutable terminal or refunds unknown execution credit.
7. Independent CANCEL races keep their own exact pending/final buffers, deadline
   and semantics. UPDATE confirmation consumes no CANCEL proof or vice versa.
   CANCEL_CONSUMED still requires actual drain and a NEW worker UPDATE wire in
   the unaccepted semantic slot; only its eventual accepted/fixed terminal can
   lead to this confirmation. TERMINAL_FIXED never accepts the rejected payload.

## Pass 3 — bounded ownership and consistency audit

Existing per-attempt outbound UPDATE final receipt copy retains its exact outer
wire and bytes while child cleanup runs. After the answered UPDATE is consumed,
its existing pending arena can hold the one confirmation frame; no simultaneous
second ordinary update is allowed after accepted terminal. Its fresh wire comes
from the existing lane's reserved cleanup identity budget, checked before attempt
admission. Counters never wrap or reset on physical reuse. Keep attempt identity
and final receipt correlation until confirmation copied-send completes; then
native endpoint TX owns the immutable frame. Worker scalar retirement is not
coordinator receipt-consumption proof and cannot set the parent's state remotely.

Coordinator's existing inbound UPDATE final original/reply copy validates the
confirmation before reclamation. Its retired wire watermark includes the worker
confirmation wire, which is later than the answered UPDATE wire in that SAME
sender stream. Distinct coordinator receipt IDs are never copied into a worker
wire watermark. Attempt/group records retain factual terminal metadata and normal
lease/parent-save state after byte-cache retirement when those remain needed.
No new struct field, reservation table, public function, transport opcode or
receipt version is required. Bodies must implement these ownership distinctions
without treating terminal_acked or parent_result_saved as interchangeable proof.

Error traces: wrong lane, nonempty payload, wrong sender/native binding, stale
attempt, reply_to naming original UPDATE instead of receipt, or mismatch with the
retained terminal receipt refuses confirmation before mutation. A cleanup error
(including consuming close returning failure) sends no success confirmation and
retains first failure/native-loss policy. Local owner_peer0 attempts use actual
local cleanup and registry lease semantics and never synthesize this remote
exchange. Identity exhaustion before admission refuses new work; inability to
finish an already retained final control exchange remains explicit failure,
never a fabricated ACK or dropped immutable terminal.

Final consistency pass: the 55-byte receipt and CGA1 sequence/prefix/dispositions
are unchanged. ACCEPTED still requires independent copy+local semantic ACK;
TERMINAL_FIXED still preserves the prior terminal rather than accepting the
pending payload. Parent TOOL ACK still proves durable result save only. Empty
COMMAND_ACK is overloaded solely by this authenticated UPDATE-lane mapping;
ordinary COMMAND/CANCEL ACK flows, unrelated lane retirement and UI HELLO caches
remain governed by their existing contracts. A valid one-way confirmation has no
response to lose, so no recursive confirmation cache or ACK-of-ACK is introduced.
Independent review is required before either coordinator or worker uses this
mapping for retirement. Subsequent actual runtime tests must cover lost final
receipt, confirmation AGAIN, parent-save ordering, cancellation and native loss.
