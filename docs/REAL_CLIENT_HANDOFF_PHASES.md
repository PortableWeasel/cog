# Exact application handoff phase exchanges

Clarification ticket5b90444. This narrows the existing application-private CGH1
role/phase interpretation; it adds no opcode, field, signature, constructor or
library authority. The existing public CGH codec continues to validate structural
shape only. O4 source bodies must await a different independent final review of
this clarification. Existing HEADLESS_CONTRACTS staged phase inventory and
HEADLESS_PASS2 scenario17 retain their operation ordering.

## Pass1 — authenticated operations and replies

All rows use CGL1 HANDOFF in HANDOFF lane, nesting the exact CGH1 body. The
coordinator is the sole initiator. Each request has a fresh nonzero wire identity
within its full existing stream key. Each response has its own wire identity and
outer reply_to equal to the exact request wire. Send success is only copied
transport acceptance: it NEVER advances the phase that requires an actual reply.
Source/target worker records must retain their original native authenticated
bindings; a replacement process/connection cannot answer an outstanding request.
For a trusted local endpoint the coordinator directly calls the corresponding
execution operation and checks its actual return instead of fabricating wire ACK.

| Coordinator request to | CGH request | Actual receiving operation | Successful CGH response |
|---|---|---|---|
| current source | QUIESCE, checkpoint0 | execution_quiesce freezes and saves one guarded exact checkpoint | CHECKPOINTED with that actual token |
| staged target | CHECKPOINTED with exact token | execution_new(STAGED), using the already copied/acknowledged purpose3 handoff profile and exact expected/next/token | PREPARED only after successful exact load and pure preflight |
| frozen source | PREPARED with exact token | execution_release_source with its retained checkpoint, after coordinator checked target PREPARED | PREPARED echo only after actual successful guard release |
| prepared target | COMMIT with exact token | execution_stage_guard after coordinator's actual exact owner CAS succeeded | READY only with the acquired normal guard, still staged |
| guarded target | ACTIVATE with exact token | execution_stage_activate after coordinator's actual exact route publication | ACTIVATE echo only after successful admission-state activation |

A PREPARED target response is NOT the source-release response. Their request
wires, receiving managed identities, actual peers and phase contexts differ.
Coordinator checks all of them before setting prepared or source_released.
COMMIT is exclusively target guard acquisition; it never tells source to release
or proves CAS occurred by itself. CHECKPOINTED is a source response when replying
to QUIESCE and a target preparation request when sent by the coordinator. The
static authenticated role/phase table disambiguates these existing kinds; the
codec alone never does so.

The profile is the existing dedicated secret BOOTSTRAP(purpose3) exchange. Its
successful copied ACK proves only that profile construction/validation finished;
it is not PREPARED, READY or guard authority. Its immutable original probe,
configuration generation and inherited reload count must agree with the
coordinator's admitted handoff. Target preparation uses the exact CHECKPOINTED
session/expected/next/token; it never loads latest or replays startup inputs.

The coordinator advances only this sequence: actual CHECKPOINTED; actual target
PREPARED; actual source release/its correlated PREPARED echo; one exact CAS at
that checkpoint; actual target READY; exact publish_owner; actual ACTIVATE echo.
It does not forward fresh commands or drive target ordinary work before that last
acknowledgment. Source stays frozen after quiesce regardless of later failures.

## Pass2 — duplicate, pressure and ordering traces

1. Local source to remote target: direct quiesce yields tokenT while retaining
   source guard. Purpose3 copied ACK is followed by CHECKPOINTED(T) to target.
   PREPARED must name that request and native target. Direct source release(T)
   returning OK permits exact CAS; COMMIT/READY, route, ACTIVATE/echo follow.
   No transport ACK is invented for the local source.
2. Remote source to local target: QUIESCE/CHECKPOINTED yields T. Local staged
   constructor consumes only its resolved candidate and exact T without a guard.
   Coordinator sends PREPARED(T) to the source and waits for the source's actual
   release echo. An old target PREPARED or send-OK cannot permit CAS. Actual local
   stage_guard after CAS permits route; actual local stage_activate completes
   activation. Source remains frozen under its retained one-shot execution state.
3. Remote source to different remote target: two PREPARED responses are retained
   in different full stream keys and matched against distinct pending request
   wires. Even numerically equal wire IDs cannot cross those peer/key boundaries.
   A forged target echo addressed as source is rejected before any authority bit.
4. Lost source-release response: source already consumed its guard and retained
   success. Exact PREPARED retransmission replays the same immutable response;
   execution_release_source's idempotent exact state does not reacquire or save.
   Coordinator does not CAS before receiving that correlated response. A changed
   checkpoint/owner/body under the same wire is a conflicting duplicate.
5. Lost target preparation, READY or ACTIVATE response: exact request retry uses
   existing pending bytes/wire and original deadline. Receiver first checks its
   retained exact request/result; it does not construct another target, acquire
   another guard, save, restore input or begin a second drive. Lost ACTIVATE echo
   cannot cause another coordinator CAS or route publication phase. Activated
   target still receives no fresh coordinator input until the echo is accepted.
6. Pressure: AGAIN copied-send accepts nothing, retains exact pending bytes and
   original absolute deadline. The next service slice retries one existing frame.
   Other control/drain lanes continue. No loop waits for a remote response. A
   response cannot be consumed before its bytes/correlation are independently
   retained. Retired unknown wires are uniformly stale; no erased proof is
   reconstructed by reading the latest owner or checkpoint.

At each receive, authenticated coordinator/source/target role and exact current
phase precede operation dispatch. Runtime incarnation, managed identity, original
peer binding, root session, source/target peer tuple, expected owner, next owner
and immutable checkpoint agree with the retained handoff. Initial QUIESCE alone
has token0; only its correlated CHECKPOINTED response introduces nonzeroT. Every
later request/reply echoes T. PREPARED from target authorizes only the coordinator
to request release, not the source to accept an unauthenticated target command.

## Pass3 — failure, authority and lifetime audit

A mismatched or late reply never advances a phase. An exact known duplicate may
replay its existing reply; a retired phase without retained proof is refused.
The sender cannot turn a receiver refusal into successful release/guard proof.
A FAILED response echoes the exact request correlation and current CGH tuple;
it is a failure, never a source-release or target-readiness acknowledgment. The
first actual failure remains primary while owned cleanup continues.

Source quiesce/save failure freezes source and may leave its guard retained;
no CHECKPOINTED success is emitted for an unknown save outcome. Target prepare
failure leaves source frozen and guarded. Source release refusal retains the
source guard and cannot emit successful PREPARED echo. After successful release,
CAS mismatch/error never reacquires the old source epoch. INDETERMINATE CAS stays
frozen without inferring commit from owner_read. Target acquisition failure or
missing READY cannot publish a route. Route failure cannot activate target.
ACTIVATE timeout cannot thaw source, repeat startup, or authorize new input by
assuming the target observed the request.

ABORT retains the same exact handoff tuple/token and cancels the receiving
side's owned handoff resources; it does not mean rollback or immediate cleanup.
Source stays frozen whether or not CAS occurred. Actual guard release waits for
its normal borrow/cleanup preconditions. Target cleanup drains any owned work
and releases resources under existing checked execution close ordering. A
transport send/receive or process reap alone is not proof all descendants or
borrowers ended. No abort acknowledgment is used to manufacture a successful
phase transition. Failure recovery remains checked host shutdown/close followed
by explicit fresh-epoch resume or fenced recovery, as the approved N profile
already requires.

Identity exhaustion is checked before quiesce and before any fresh phase wire
that would consume reserved final/cleanup identities. A request/reply never wraps
or resets a lane. Timeout is the original admitted minimum; a duplicate cannot
extend it. The source and target apps, their configuration/probe owners, native
records and dedicated immutable pending/reply buffers remain owned until their
actual use/cleanup ends. No raw event pointer escapes the ingress lease.

Consistency audit: unchanged public CGH values and literal encoding; COMMIT
still exclusively permits target acquisition, READY still means guarded/staged,
route still precedes ACTIVATE, and source is never thawed. No new body fields or
allocation are needed: handoff phase/flags and existing per-executor HANDOFF
pending/final caches hold the exact exchanges. The shared handoff coordinator's
pending/reply buffers are nonsecret; purpose3 credentials stay in the dedicated
profile reservation with its original probe and full-capacity wiping. A approved
narrow prose edit to handoff.h is the sole shared-header delta. All implementation
source in earlier commits is unrelated group/mailbox work, not dependent handoff
body acceptance.

Verification:12/12 strict standalone/repeated/combined/reversed handoff and
composition include checks pass GCC14/Clang19 C11 and G++14/Clang++19 C++11.
Evidence is in impl-headless-coordinator/build-handoff-headers, including each
actual source and compiler log. These are declaration checks, not runtime phase
or whole O4 acceptance. Different independent review remains required before
coordinator/worker bodies may depend on this clarification.
