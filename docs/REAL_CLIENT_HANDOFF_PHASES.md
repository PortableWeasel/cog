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
