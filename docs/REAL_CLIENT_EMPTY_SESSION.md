# Empty durable session admission

Bug `cd3d092`, baseline `84e2272`. A fresh durable constructor admits owner/guard
but writes no checkpoint until first input. Exit before input leaves a valid
owner-only manifest: NEW refuses its ID and RESUME/FORK/RECOVER all fail legacy
load with NOT_FOUND. The existing RECOVER mode cannot repair it. This design
changes only application admission and uses existing G store APIs.

## Pass 1: narrow contract

Successful fresh DURABLE construction commits the empty session snapshot before
publishing the app. It contains its exact ID and no nodes, active leaf, tool ledger
or model/provider provenance. J still resolves current immutable configuration
through its normal startup sources; saving the empty session neither persists
credentials nor makes up historical configuration. Disabled store/LIST remain
unchanged. A later provider/core constructor failure may leave a committed empty
session, which explicit RESUME can load safely.

The crash/failure window between owner publication and first checkpoint still
needs recovery. Explicit RESUME may reconstruct only a valid nonzero owned
checkpoint0 reservation with no replay-loadable data. Existing public calls:

1. owner_read validates the authoritative owner/checkpoint; both zero is absent.
2. load_ex(REPLAY) must return NOT_FOUND; every other load error is preserved.
3. session_new constructs an empty matching-ID session within current reservations.
4. owner_compare_exchange uses the exact old owner/checkpoint0, fresh incarnation
   and checked epoch+1. Locked CAS excludes live guards/stale state and refuses
   legacy state, including an incomplete legacy pair.
5. owner_acquire confirms the newly admitted normal owner under the lock.
6. Guarded save commits the empty checkpoint before app construction succeeds.

No owner/data deletion, epoch reset, unowned adoption, effect replay, fabricated
result or implicit recovery-mode downgrade occurs. No new API/option is added.
The existing public RESUME prose gains this precise no-checkpoint exception;
NEW collision and FORK/RECOVER semantics remain intact.

The private `session_admission.h` owns the existing sessions_open declaration and
documents partial construction, exact authority and all error boundaries. No
implementation body is changed in this pass.
