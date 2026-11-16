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

## Independent R1 correction and fresh rerun

Review 8ffb926 cycle 1 rejected 8549003: an authenticated oversized frame set
the endpoint's LIMIT failure while a full descriptor queue let core_step return
OK and dispatch an ordinary event. Bug 23e3a16 records the correction under the
already approved fail-closed contract. Critic regression c306e02 was imported as
4f36bee, including lifecycle-observer begin/end/error/free reentry and manual
service-error queued-work preservation checks.

35c7049 checks the service result before CONNECTED or receive reservation
admission: non-OK/AGAIN/IO failures retire the exact endpoint and immediately
return the first fatal error. Ordinary IO/EOF still drains retained complete
frames normally. Already queued descriptors keep their copied proof/payload
owners after endpoint retirement. The core driver latches the failure before
ordinary dispatch; explicit rebegin cannot rediscover the retired fatal peer.

c81ffc3 extends the existing proof regression across both descriptor and arena
pressure for LIMIT (oversized declared frame) and PROTOCOL (partial prefix then
EOF). All four assert fatal/done, zero dispatch, retained preexisting payload
ownership, retirement and clean rebegin. Existing ordinary EOF cases remain
enabled. 9bce457 only corrects the nearby explanatory comment.

Fresh affected target rebuilds and tests passed after the correction: GCC 10/10,
Clang 10/10, ASan/UBSan/LSan 10/10 and TSan 5/5. The profile/options and real
target scopes are unchanged from the table above; logs are
`build-o2-*/r1-scoped-build.log` and `build-o2-*/r1-scoped-test.log`. No failures
were skipped. Header signatures/layout remain unchanged, and the earlier strict
header validation remains applicable. Full O/P build limitations remain as
recorded above.

Independent re-review of this final correction remains required; these are
writer results, not acceptance. No live provider calls, main changes or
integration were performed.
