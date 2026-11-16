# O3 bounded orchestrator writer evidence

Parent d5280b7; runtime dff0484, codecs 9ccb5d5, fixture migration f268a60.
Runtime started from integrated O2 d17c924. Approved private layout gate
fa9fc05/21a4a04 and public N contracts govern the implementation. Approved O8
f72dbbe is merged at b41d10d. No main branch or paid provider calls were used.

## Runtime and ownership

The obsolete blocking text/PID protocol is replaced with actual nonblocking Q
service and binary CGW1. Spawn returns a fresh strong managed identity before
connection, with fixed argv, C+2 physical executor scalar, minimal PATH/LC_ALL
environment and unblocked child INT/TERM. Only immutable native fork PID plus
kernel PID/UID and exact CGW HELLO can bind one actual fabric peer. No unowned
connection or text PID becomes managed authority. Pure CGW1/CGH1 codecs have
independent review at b9eb766 and are integrated as 475f6f9.

Constructor-reserved workers, links, ingress frames, event descriptors/byte ring,
control FIFO, route records and executable slices never grow in service. Three
control transitions and event identity holds per worker remain independent of
ordinary queue/byte pressure. Retained event data/proof is copied before endpoint
retirement; copy/ACK is class-ordered, transactional and wipes payload storage.
Unknown authentication attempts have bounded handshake/HELLO lifetime.

Exact-PID WNOHANG checks precede every signal. ECHILD permanently disowns and
reports ownership loss; no waitpid(-1), foreign reaper, hidden blocking join or
old-PID reuse is used. Stop/shutdown keep wire and effective kill deadlines
separate, advance despite ordinary pressure, and preserve the first driver
error. A new all-child shutdown does not inherit an expired timer from a child
already terminal before admission. Done means native lifetimes are disposed;
retained endpoint/event/route retirement can still block release_worker.
Auto replacement waits for that old lifetime to retire and creates idle capacity
with a fresh identity; it never replays tasks or drops affected route diagnosis.

Fenced route publication checks the exact durable owner/checkpoint against the
borrowed store and a healthy authenticated remote peer or trusted local zero.
Routes remain advisory, with exact-owner removal; no guard, CAS or application
handoff transaction is invented in the library.

## Meaningful checks and corrected fixture assumptions

The rewritten unit executable spawns real copies of itself as fixed-argv workers.
It verifies startup proof, environment/mask, capacity/placement, queued shutdown
ACK identity, shortened effective deadline, noack/pressure cleanup, preauth stop,
postpublication exec failure, stale IDs, exhausted worker/event identities,
mandatory control delivery, copied proof after reap, ECHILD and unrelated-child
ownership. Constructor and accepted-endpoint allocation sweeps fail every allocation;
accepted failures assert NOMEM, zero link/worker growth, exact FD/socket cleanup
and preserved primary error through shutdown completion. Wrappers forbid
allocation during established service, events and shutdown. Actual fenced-store
route tests exercise guard acquisition, checkpoint save, owner CAS, publication
and stale tuple rejection. Low-quantum tests retain unvisited handshake deadlines
and expire silent proof-only links.

Six M component exclusions now execute real production paths: unit-orchestrator,
integration-orchestrator-ipc/stress, and original scenarios07/09/10. The migrated
aux fixture uses nonblocking connect/service and kernel-parent proof before CGW
HELLO, never inherited environment control. Test behavior is admitted through
copied FXM1 application frames after READY with actual echo receipts.

Initial execution exposed fixture assumptions: recv before handshake publication,
release immediately after native reap, and replacement without ACKing a later
DISCONNECTED event. These were corrected with actual service/copy/ACK loops,
without dropping outcome assertions. A failed replacement could previously
continue with zero managed ID/PID into a raw SIGKILL. The observed exit137 is
consistent with that process-group signal; it is not claimed as confirmed OOM.
All signal adapters now require a positive exact owned PID and valid managed
record; failed spawn/readiness/replacement drains owned children and exits before
continuation. Unit deliberate reap-loss injection has the same positive-PID guard
at 2601b54. No signal operation is permitted on PID0 or an unknown record.

Native fixtures explicitly reserve frame256, ordinary bytes4096, events8 and
retain the original concurrent worker counts. Measured actual type sizes give
at most59740 bytes of orchestrator arrays/path,92232 for listener+8 accepted
endpoints, and13560 per child endpoint:260452 aggregate bytes at8 workers,
excluding executable/sanitizer runtime. Measurement source is
build-o3-checkpoint/reservations.c. All test sockets/builds/logs/TMPDIR stay in
checkout build directories; no /tmp build tree or log is used.

## Fresh validation

Exact recovered shipped curl/c-ares arguments are read from
integrate-headless/build-dependencies/cmake-args.txt. Debug/Werror, serialized
profiles and at most -j2 builds were used.

| Profile | Build | Runtime result | Logs |
|---|---|---|---|
| GCC | Full default targets including strict public headers | Seven component tests pass | build-o3-gcc/full-build.log, components-test.log |
| Clang | Full default targets including strict public headers | Seven component tests pass | build-o3-clang/full-build.log, components-test.log |
| GCC ASan/UBSan/LSan | Real seven-test dependency targets and aux executable | Seven pass; detect_leaks=1 and halt_on_error=1 | build-o3-asan/components-build.log, components-test.log |
| GCC TSan | Same real seven-test targets, including threaded broadcast scenario | Seven pass; halt_on_error=1 | build-o3-tsan/components-build.log, components-test.log |

The full component runs are at b41d10d. The final narrow unit PID-guard addition
2601b54 is rebuilt and rerun in every profile (guard-build.log/guard-test.log).
Independent critic accepted-endpoint fault sweep349fd98 is imported as6b7317d
and the changed unit is rebuilt/rerun in all four profiles
(accepted-build.log/accepted-test.log); all pass.
The wire target links the real production archive and its aborting allocator
wrappers; no runtime replacement, skipped assertion or fake library is used.
Earlier failure logs remain for diagnosis rather than being relabeled as passes.

Independent runtime review235ca88 remains required. These component scenarios
retain their assertions but do not claim O9's future joined O4 app/executor
composition. Whole O/P, installed-worker end-to-end and all38 release acceptance
remain separate gates. No integration or parent completion is claimed here.
