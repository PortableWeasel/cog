# O2 recovered cooperative core validation

Source tickets e55f22c (core.c), 959efdc (agent.c), parent 99c42ec;
independent review 8ffb926. Recovered body 64386bc is preserved. Public allocation
clarification b55dd9e passed independent review at 44fd170 after three targeted
passes. The source validation below is fresh post-OOM evidence at 8549003;
pre-OOM reports do not substitute for these checks.

## Implemented behavior and meaningful oracles

The recovered implementation uses one bounded engine for run/manual driving,
checked persistent monotonic history, finite readiness-derived waiting outside
manual slices, callback reentry protection, first-error/stop completion and
injected FIFO restoration. Exact child session borrows block record release and
require detached child cleanup before return; queued intent disposal releases
leases without executing effects. Accepted and attached endpoints retain copied
native proof acquired before further IO, independent of event/arena capacity.

Existing cooperative tests cover run/manual ceilings, service fairness under
continuous queues, callback reentry, invalid-call output preservation, clock
failure/backwards/overflow, driver end/rebegin, signal stop and exact child
borrow/refusal/return/release. Existing core tests include actual pthread stop
and injected pending-message restoration. Proof tests use real Unix sockets,
kernel credentials, saturated event/arena reservations, retained frames after
EOF, cached proof across rebegin, generic versus protected ingress and idle
writable readiness. Existing agent/IPC/service suites remain real library tests.

8549003 extends the existing allocator test rather than adding a replacement
suite: every accepted-endpoint constructor allocation fails in turn, propagates
NOMEM/done through end, publishes no peer/event and closes the accepted socket.
The success case compares allocation count with the named endpoint constructor,
then forbids allocation during stop/end/rebegin and verifies retained ownership.
Final descriptor counts balance; ASan/LSan verifies heap cleanup. The existing
core-constructor sweep and forbidden driver/borrow-growth checks remain enabled.

## Fresh validation at 8549003

All builds use the exact recovered shipped curl/c-ares profile from
`integrate-headless/build-dependencies/cmake-args.txt`, Debug, Werror and at most
`-j2`. Builds, logs and compiler TMPDIR are checkout-local build directories.

| Profile | Real targets/tests | Result | Log directory |
|---|---|---|---|
| GCC strict | core, agent, core-service, cooperative, proof, reservations; IPC baseline/critic/nonblocking/readiness; public header smoke | 10/10 scoped CTest pass; headers compile | build-o2-gcc |
| Clang strict | same targets and tests; public header smoke | 10/10 scoped CTest pass; headers compile | build-o2-clang |
| GCC ASan + UBSan + LSan | same 10 runtime tests | 10/10 pass; detect_leaks=1, halt_on_error=1 | build-o2-asan |
| GCC TSan | core, agent, cooperative, proof, reservations, including pthread stop | 5/5 pass; halt_on_error=1 | build-o2-tsan |

Each directory retains configure.log, scoped-build.log and scoped-test.log.
The GCC full default build was attempted and failed on the unfinished O3
orchestrator.c worker-ID header/body signatures (build-o2-gcc/build.log).
That source belongs to dff0484/d5280b7. It was neither changed nor stubbed here.
The Clang build was scoped to real O2 dependencies; no whole-project pass is
claimed. Whole O/P all-target and joined scenario validation awaits O3/O4/O9.

## Remaining independent gate

These are writer results, not independent acceptance. The critic is checking
lifecycle-observer reentry and whether a saturated queue/arena can defer a fatal
IPC service result contrary to the approved first-error contract. No interface
weakening is proposed; a confirmed regression requires a source correction and
fresh affected validation before acceptance. No live provider calls, main
changes or integration were performed.
