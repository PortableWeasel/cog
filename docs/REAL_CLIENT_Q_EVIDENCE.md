# Stage 14.Q writer evidence

Ticket `bcd9c0a`; approved E base `b1bd6093ba47330b52efaba0ae82cc900eaf0c6f`.
Managed worktree `impl-scheduler`, branch `codex/impl-scheduler`.
Writer implementation is ready for independent review; Q is not closed.

Children: Q1 `7b15c53` nonblocking IPC; Q2 `467ffb9` fixed core reservations;
Q3 `c1331d9` fair scheduler; independent review `fc23bfd`; integration `76325f3`;
finalize `4bac201`. Review/integration/finalize remain open until their evidence
is complete. Remote agent/replay/handoff application integration remains N/O.

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

The full baseline cog-core target fails strict compilation in the separately
owned pre-migration session_store.c positional vtables, whose new load_ex fields
are missing. Q does not suppress warnings or edit that module. After G lands,
register the two new normal tests against real libraries and rerun core/store
integration without the isolation double. Root/shared tests CMake remains with
the coordinator; scripts/test-q-runtime is the current reproducible fallback.

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
Independent Q review must confirm their consistency before integration.

Final writer run: all six executables pass strict GCC and strict Clang, and
separate ASan+UBSan (leak detection/halt enabled) and TSan (halt enabled) builds.
The final runs include fixed local agent/task/status/result capacity checks and
two independent ID1 fabrics. `git diff --check` passes. The coordinator reviewed
and approved the identity header/scenario consistency amendment before commit;
this does not replace independent implementation review.
