# Guard release ownership on failure

Design ticket d97a81b, reproduced defect fae1e27. Header-only clarification;
no function signature, struct layout, allocation, opcode or vtable slot changes.

## Pass 1: one result classification for public and backend guards

For a valid live guard, INVALID_STATE is the only nonconsuming refusal. Active
public borrowers are checked before the backend callback. A backend returning
INVALID_STATE must likewise leave its admitted handle/resources intact. After
borrow return or correction of that refusal, another release may be attempted.

OK and cleanup errors consume the exact backend handle and public wrapper once.
In particular, native close IO must not leave a dangling public alias or make
callers retry a numeric descriptor which may already have been released/reused.
The backend completes best-effort teardown and reports its actual error; the
wrapper unlinks/frees its bookkeeping under the same classification. A NULL
public guard is an OK no-op. Invalid/dangling foreign pointers are outside the
live-handle precondition, not additional ownership-return cases.

The error remains factual. Consuming a handle does not prove successful lock
release, target preparation, CAS authority or handoff completion. A frozen source
clears the consumed alias, retains failure and never reports successful release
or resumes the old epoch. Checked destruction proceeds through remaining owned
resources after consuming cleanup errors, preserving the first error.

This is an explicit semantic amendment for custom fenced backends, not a claim
that old retain-on-IO callbacks already conform. They must adopt the same result
classification before use with the amended wrapper. No wrapper can infer backend
consumption from a pointer, backend identity, or a second release call. No ABI
layout change or automatic adapter hides this compatibility obligation.

## Pass 2: resource and custom-backend traces

1. **Borrow refusal.** Acquire guard G, borrow it for live execution, request
   release: INVALID_STATE, no backend release and no resource mutation. Return
   the exact borrow after execution destruction, then release G once. A backend
   refusal likewise retains its handle. No error classification guesses that a
   borrowed guard was consumed.
2. **File close failure.** Quiescent G is unlinked, its native close runs once,
   and the storage is freed. Close reports IO even if the descriptor was already
   consumed. Public release returns IO with G consumed. Caller clears its alias,
   does not retry close/release or touch freed bookkeeping, and preserves IO.
3. **Custom cleanup failure.** Custom callback tears down its backend guard and
   returns IO (or another cleanup error). Public wrapper removes/frees its own
   guard under the same rule. Callback count stays one; store destruction does
   not repeat the release. A custom INVALID_STATE refusal instead keeps both
   owners intact. Constructor unwind uses the same consuming callback contract.
4. **Partial app disposal.** Owned tool/delegation/workspace/provider/config
   resources are consumed in declared order and each alias immediately cleared.
   Consuming guard IO is recorded; remaining observation/store/session/buffers
   are still torn down, outer app is freed and output NULL. Earlier primary error
   still wins. A late nonconsuming guard refusal retains a cleanup-only app and
   that live guard; already-consumed aliases remain NULL. This distinction cannot
   be inferred from return code when an earlier primary error masks refusal:
   the checked app destructor's output pointer reports remaining ownership.
   Preflight live/undrained refusal remains untouched and unconsumed. A retained
   partially closed app never resumes execution; later call may only finish its
   remaining destruction after the guard refusal has been resolved.
5. **Source handoff.** PREPARED target precedes release. If release consumes G
   but returns IO, source clears G, retains failure and stays frozen; it does not
   set successful-release evidence, advance owner CAS or send a successful
   PREPARED release receipt. Exact retry must not call release on G again or
   convert missing G to OK. Subsequent close consumes other resources. Failure
   before CAS never authorizes old source thaw; failure after CAS never changes
   the durable epoch back.
6. **No current lease confused with an old one.** Numeric FD reuse after close
   cannot cause a second close by the caller. Custom handles do not borrow file
   implementation layout. No guard alias survives under a different backend or
   new owner epoch, and NULL-release remains an independent no-op, not evidence
   that a previous failed release succeeded.

## Pass 3: consistency and compatibility audit

| Boundary | Required ownership/error behavior |
|---|---|
| NULL public guard | OK no-op, no callback, no inferred prior-release success. |
| Valid borrowed guard | INVALID_STATE, still live, callback not entered; existing borrow count unchanged. |
| Valid backend refusal | INVALID_STATE consumes neither backend guard nor wrapper; caller retains authority only under prior frozen/borrow constraints. |
| Successful cleanup | Both handles consumed once, caller aliases cleared, successful release may be recorded. |
| IO/other cleanup error | Both handles consumed once, original error retained; no repeat release, execution thaw or successful handoff proof. |
| Custom backend returning AGAIN | Not a deferred-release protocol: as a non-INVALID_STATE error it consumes. Backends must quiesce asynchronous work before this destructor. |
| Earlier app primary failure | First error wins; app output pointer still truthfully distinguishes complete consumption from retained cleanup-only ownership. |
| Partial app cleanup refusal | Every consumed pointer is NULL; remaining owner/guard cannot run work, but may complete cleanup after refusal resolution. |
| Exact source retry | Existing frozen failure remains failure; a cleared consumed guard never grants release success or CAS. |
| Constructor rollback | Backend handle construction failure creates no owner; wrapper allocation failure after successful acquire invokes the same consuming release discipline. |
| Context/store destruction | Store outlives guards; consumed guards are absent from its bookkeeping even on cleanup error, so later store free cannot revisit them. |
| ABI and migration | No slot/signature/layout/version changes; nevertheless custom callback error semantics are explicitly amended and require audit/conformance updates. |

The scenario coverage is scenario05 graceful teardown, scenario13 durable store
ownership, scenario17 migration, and scenario20 cancelled/drained work. Existing
static backend dispatch remains unchanged. No allocator, secret lifetime, output
buffer, wire protocol or retry timer is added. Native close errors are preserved
without assuming file-specific internals for custom backends. A backend which
retains a handle on IO is nonconforming after this amendment; caller-side probing
or silently replaying its destructor is not an adaptation mechanism.

Validation: session_store.h, execution.h, execution_internal.h and
composition_internal.h passed16 strict standalone checks: GCC/Clang C11 and
G++/Clang++ C++11 with Werror. Commands/results are retained under
build-store-release-contract/checks.log. This is a header-only gate: source fixes
and real file/custom-backend fault regressions remain required after independent
approval. The existing fae1e27 failing regression is not claimed fixed here.
