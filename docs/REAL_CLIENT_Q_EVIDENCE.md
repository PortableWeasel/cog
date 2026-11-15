# Stage 14.Q delivery evidence

Ticket `bcd9c0a`; approved E base `b1bd6093ba47330b52efaba0ae82cc900eaf0c6f`.
Managed worktree `impl-scheduler`, branch `codex/impl-scheduler`.
Revision `769c6d584932f890d30c8d9064b0d63f8ec24dde` passed independent review
on cycle 2 after one revision. It was merged into main without conflicts as
`6b49e0486b812b71697f1a43a3f14cec0fb974cc`, preserving the approved F transport.
Integration ticket: `273a7f9`. Full combined G validation remains `b8a6fc1`.

Children: Q1 `7b15c53` nonblocking IPC; Q2 `467ffb9` fixed core reservations;
Q3 `c1331d9` fair scheduler; independent review `fc23bfd`; integration `76325f3`;
finalize `4bac201`. Q implementation, independent review and scoped integration
are complete. Remote agent/replay/handoff application integration remains N/O.

## Implemented behavior

IPC has fixed copied send queues, retained input slots, incremental four-byte
frames and eight-byte assigned identity, finite handshake deadline, bounded
nonblocking service, transactional broadcast and fixed copied routes. No runtime
array/buffer growth or global mutable state is introduced. Accepted endpoints
are explicit inherited-limit constructors and release through endpoint_free.

Core reserves IPC peer tracking, payload arena, ingress descriptors, per-session
pending FIFOs and local agent copied fields at construction. Its private IPC
boundary exposes role and one-unit driver without exposing layout or linking
core to net/provider/toolkit. A separate IPC descriptor reserve derived from
ipc_service_quantum prevents a continuously refilled external queue from blocking
all IPC admission. Queued descriptor bound is external event capacity plus that
IPC reserve; all ingress payload bytes remain within ipc_payload_bytes. Both
sources rotate dispatch and preserve their own FIFO order.

Service hooks run before first dispatch, deadlines, idle and within the dispatch
interval under sustained work. Idle budgets use the earliest deadline and the
configured maximum. Stop/error propagation and stop-before-pending-injection
ownership are preserved. Larger-than-core-ceiling frames close only their peer
and return LIMIT, retaining a queued disconnect event for the next run.

## Reproduction

```sh
cmake -S . -B build-q -DCOG_ENABLE_WERROR=ON
cmake --build build-q --target test_ipc test_ipc_critic -j4
build-q/tests/unit/test_ipc
build-q/tests/unit/test_ipc_critic
scripts/test-q-runtime
CC=clang scripts/test-q-runtime
Q_SANITIZER=address ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
  UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 scripts/test-q-runtime
Q_SANITIZER=thread TSAN_OPTIONS=halt_on_error=1 scripts/test-q-runtime
```

The six executable suite is strict under GCC and Clang. It includes migrated
functional/critic IPC tests; direct nonblocking fragmentation, retained overflow,
atomic broadcast, queue ownership, 1 MiB slow-peer partial writes, timeouts and
checked arithmetic; legacy core dispatch/pthread stop/stop-after-inject tests;
continuous full-queue service/cancel/IPC fairness and fixed FIFO/agent limits;
and listener/connecting core IPC integration with backpressure and failure.
The script links an explicit assert-failing store test double only to isolate
scheduler tests while G migrates the store archive. No fixture supplies a store;
this tests no persistence behavior and adds no production success stub.

## Limits and remaining integration

The full baseline cog-core target still fails strict compilation in the separately
owned pre-migration session_store.c positional vtables, whose new load_ex fields
are missing. Q does not suppress warnings or edit that module. After G lands,
run the two registered normal tests against real libraries and rerun core/store
integration without the isolation double. The normal `test_core_service` and
`test_ipc_nonblocking` CMake targets/CTest entries are now registered separately
from pending G additions. Neither target includes the isolation double.
`scripts/test-q-runtime` is the current reproducible scheduler isolation check.

Existing orchestrator and scenario peer drivers still need N/O's nonblocking
migration. Public remote agent update/query/ack/release functions are still
unimplemented, intentionally left to N/O rather than represented by success
stubs. Q tests cover the fixed local reservations used by those later APIs.
Full original20/R01–R18 release conformance is not claimed here.

The coordinator approved correcting ipc.h's obsolete introductory blocking prose
to match its already-approved nonblocking function contracts. No signature,
member, return-code declaration or layout contract was amended by Q.

Identity-scope gap `ee411f9` corrects obsolete process-wide uniqueness prose:
peer IDs are monotonic within one listener lifetime; cross-fabric identity is
(owning endpoint/fabric, peer_id). The scenario09 amendment and direct two-fabric
ID1 routing fixture accompany this coordinator-approved documentation correction.
Independent Q review accepted this identity correction and fixture.

Final writer run: all six executables pass strict GCC and strict Clang, and
separate ASan+UBSan (leak detection/halt enabled) and TSan (halt enabled) builds.
The final runs include fixed local agent/task/status/result capacity checks and
two independent ID1 fabrics. `git diff --check` passes. The coordinator reviewed
and approved the identity header/scenario consistency amendment before commit;
this does not replace independent implementation review.


## Review cycle 1 revision 1

Independent review rejected writer tip cfdbbdd and added permanent regressions
in 8385d89 for pending auto-injection/public-dequeue aliasing and a stale IPC
handshake deadline after reuse. The pending flaw could duplicate/drop prompts
and underflow an emptied owner's count; the reconnect flaw rejected a new
attempt after the old deadline.

The revision moves an injected item out of the logical FIFO after successful
enqueue. Its independent copied event bytes and an explicit owner/restore-slot
lease keep public dequeue/count/queue coherent. Draining all remaining items or
adding replacement/other-session items cannot mutate its identity or steal its
restoration capacity. Stop/error before dispatch restores that exact copy at the
front; normal dispatch releases the lease without touching a mutable FIFO head.
Regression coverage drains one/all remaining entries, requests owner reuse,
queues replacements to capacity, stops/errors before dispatch, then reruns and
asserts exactly one delivery in FIFO order. The existing concurrent stop-after-
inject test remains in the six-suite run.

Disconnect now clears all connection-lifetime clock/deadline, direction and
identity/prefix state. Tests reconnect after the previous successful deadline
and after an expired failed handshake, including an independent earlier clock
origin for the new attempt. Public API declarations and shared CMake are unchanged.

Revision validation: the six executables including the permanent critic and
extended mutation/reconnect regressions pass strict GCC, strict Clang, separate
ASan+UBSan with leak/error halt, and TSan with error halt. `git diff --check`
passes. This is writer evidence for rereview, not an independent PASS or merge.

## Independent review and main integration

The independent critic inspected both fixes and ran all six executables under
strict GCC, strict Clang, ASan+UBSan with leak/error halt, and TSan with error
halt: 24 passes including both permanent critic regressions and expanded
lease/reuse tests. Public dequeue/count exclude the injected copy, its pinned
owner reserves restoration capacity, and stop/error restores exact copied bytes
ahead of replacements without touching a mutable FIFO head. Reused endpoints
clear prior connection clocks/deadlines and support an earlier new clock origin.

After merging into main, all six isolated suites passed again under GCC and
ASan+UBSan. Normal strict CMake IPC targets `test_ipc`, `test_ipc_critic` and
`test_ipc_nonblocking` built and their three registered tests passed. All 11
audited-profile F transport tests also passed. Building real `test_core_service`
still fails in baseline `session_store.c` on the two missing `load_ex` vtable
initializers; no warning was suppressed and no session/store source changed.
Check `b8a6fc1`, after G and before P, must validate real-library core/IPC/store
integration and preserve both G and Q test registrations. This delivery does not
claim full project/runtime/release conformance. No paid/provider calls ran.
