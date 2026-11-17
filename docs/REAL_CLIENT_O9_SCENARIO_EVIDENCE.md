# O9 joined scenario evidence

O9 replaces six historical canonical scenario substitutes with actual bounded
headless runtime joins. The production tree is unchanged from the accepted O4
baseline `e8c155f`. Original assertions remain separately selectable component
tests. Independent O9 package review and integration remain separate gates.

The assembled source snapshots are native `a32b0c8`, sessions `956fa26`, shared
support `fc97ce4`, and the sole-owner registration amendments. Source history in
the assembled branch preserves each test-only checkpoint; no passing result is
inferred from its name, a scripted model reply, or a manually completed agent.
Later test-only admission barriers are frozen in `eba41b9`/`3680560`.

## Actual canonical cases

All provider requests use a finite synthetic script over actual loopback TLS,
the REAL backend, an ephemeral local certificate and `synthetic-native` only.
Workspace effects run through the actual asynchronous `run_command` tool.
Native workers execute the built `cog-cli`, with authenticated observed peers,
owned positive PIDs and the actual coordinator registry/store/control paths.

| Canonical source / CTest stem | Additional mode | Actual joined oracle | Exact TLS requests |
|---|---|---|---|
| `tests/scenarios/test_03_subagent_lifecycle.c` / `scenario-test_03_subagent_lifecycle` | `-cancel` | Local/native execution, observed RUNNING, distinct real child/parent session linkage, once-only effects, terminal semantic ACK, durable parent TOOL/ledger and slot/registry retirement; repeated group gets fresh IDs. Cancellation drains its actual admitted native request. | 12 / 5 |
| `tests/scenarios/test_07_aux_crash.c` / `scenario-test_07_aux_crash` | `-crash-after` | Kill only the exact owned unreaped native worker after factual RUNNING. Before-effect provider hold and after-effect command marker distinguish observed effects; both retain admitted uncertainty, first failure, durable INDETERMINATE and actual reap. Explicit ordinary REAL resume refuses without another request/effect. | 4 / 4; resume adds 0 |
| `tests/scenarios/test_09_subagent_messaging.c` / `scenario-test_09_subagent_messaging` | none | Actual sibling `send_message`/`receive_messages` tools, observed admitted IDs, whole quoted/newline payloads and source IDs, empty polls, two fresh groups and retired-ID refusal. A full eight-entry mailbox retains the same ninth send handle/deadline until receiver progress. | 23 |
| `tests/scenarios/test_10_fan_out_fan_in.c` / `scenario-test_10_fan_out_fan_in` | `-fanout-cancel`, `-fanout-limit` | Three children on one local executor and two distinct authenticated native peers; controlled reverse completion, submission-ordered parent results, exactly-once effects, full retirement and repeated reuse. Active cancellation drains every member; insufficient result capacity refuses before STARTED. | 16 / 5 / 2 |
| `applications/cog-cli/tests/test_11_ui_reconnect.c` / `scenario-test_11_ui_reconnect` | none | Actual retained host-client state and physical sockets, real replay-ring exhaustion/GAP and atomic replacement, stable session/generation, pending ASK identity across disconnect with no implicit allowance. A genuine final reply is removed before client consumption; two observed identical copied command frames reuse one effect. An expired original command frame cannot regain controller authority. | 6 |
| `applications/cog-cli/tests/test_17_session_migration.c` / `scenario-test_17_session_migration` | `-timeout` | Four local/native moves with exact checkpoint/history/owner/route and guard-order assertions; five actual turns include a new effect after every activation. A lost genuine cleanup reply is retried with retained receipt bytes and physical slot reuse. SIGSTOP after release/CAS keeps the source frozen, without route/activation, and the worker is reaped. | 10 / 2 |

The canonical09 authorization path covers same-group siblings and an observed
retired group identity. A live foreign-group negative is separately exercised
by `component-coordinator-admission`, which calls the real mailbox implementation
against bounded test records. That component does not claim two simultaneously
executing delegated groups. The canonical07 before-effect control remains
uncertain after actual admission; absence of a file does not authorize replay.
It makes no automatic replacement, failover or external-effect rollback claim.
Canonical09 publishes sibling IDs only after both actual executor slots are
started and in STARTED phase; its Python consumer additionally waits for current
registry RUNNING facts. These are independent acknowledgements.

The cancellation controls synchronize actual admission, without extending an
operation deadline or retrying a refused constructor. The03 provider-entered
marker is combined with current registry RUNNING, first-admission-sent and
started facts, all in the wait predicate. The10
cancellation-only transport wrapper holds an unaccepted child PROFILE until its
predecessor is actually started/RUNNING in a held provider request. It checks the
same frame pointer, length, wire ID and deadline throughout the hold, observes
both exercised gates and actual accepted copies, then requires all three current
attempts active on the distinct placements before the original simultaneous
cancel requests and unchanged terminal/effect/retirement assertions. Natural
positive fan-out retains its ordinary startup path.

Migration is same-host checkpoint transfer. Cross-host/live-handle migration and
every possible filesystem/network failure boundary remain outside these cases.
The UI fixture's local IO/disconnect outcome alone is not an authority result;
its rejected old frame and unchanged control/command/effect state are checked.

## Retained component mapping

All assertions from the original six canonical sources remain in these files.
Only include relocation, comments and the old migration scratch path changed.

| Historical proof | Preserved source | CTest |
|---|---|---|
| Agent lifecycle/late-result/registry transitions | `tests/scenarios/components/test_agent_lifecycle.c` | `component-agent_lifecycle` |
| Aux-core crash/session list/optional replacement/registry recovery | `tests/scenarios/components/test_aux_crash.c` | `component-aux_crash` |
| Endpoint routing and session-parent metadata | `tests/scenarios/components/test_subagent_messaging.c` | `component-subagent_messaging` |
| Placement/broadcast/manual registry fan-in | `tests/scenarios/components/test_fan_out_fan_in.c` | `component-fan_out_fan_in` |
| Session-local buffer/queue behavior | `applications/cog-cli/tests/components/test_ui_buffer_component.c` | `component-ui_buffer` |
| File-store save/load/history/transfer | `applications/cog-cli/tests/components/test_store_transfer_component.c` | `component-store_transfer` |
| Actual mailbox capacity/cached retry/live foreign-group refusal | existing `applications/cog-cli/tests/coordinator-leaf/test_admission.c` | `component-coordinator-admission` |

Existing O4 `test_headless_native.c` and `run_headless_native.py` remain unchanged
and retain their six success/limit/single/cancel/handoff/timeout regressions.
Shared C equivalence is checked using that unchanged single-mode Python oracle;
a second smoke uses the reusable callback TLS support.

## Shared ownership and limits

`tests/scenarios/fixtures/headless_fixture.[ch]` declares test-only setup before
its bodies. It reserves a REAL host, permits explicit finite test profiles,
advances actual runtime slices, publishes read-only observed registry metadata,
signals only a WNOWAIT-proved owned positive worker PID, and preserves the first
failure through done/reap/consuming-close checks. The established runtime
shutdown deadline is unchanged. Cleanup observes its deadline plus a bounded
terminal slice, then applies a finite exact-owned-worker kill/reap fallback.

`headless_fixture.py` owns local TLS and starts a fresh dedicated Linux subreaper
supervisor for each harness. The supervisor adopts even tool descendants that
created separate sessions. It enumerates only its own immediate children,
proves ownership with exact-PID WNOWAIT, kills/reaps only those positive PIDs,
and requires no remaining children before reporting cleanup success. No cached
or reaped numeric process-group identity is signalled. Four infrastructure
controls cover harness success, exit17, SIGABRT and timeout, each leaving an
escaped-session child and grandchild; both original process identities must be
absent after the context. These controls provide cleanup evidence only.

All TLS keys, synthetic data, effect files, metadata, logs and build trees are
checkout-local under `build*`; no paid service is contacted. Actual worker cases
share the CTest resource lock `cog-native-scenarios`, so `ctest -j4` does not run
several native fixture trees concurrently. Other tests retain normal parallel
scheduling. The static test helper explicitly undefines NDEBUG in Release, as
do the canonical/component executables under the project test assertion policy.

## Writer validation

Passing leaf checkpoints establish their own cases only; they do not replace
assembled-package validation. Writer results in `impl-o9-fixture` are:

| Profile | Build / tests | Result | Retained logs |
|---|---|---|---|
| GCC14.2, default, Werror | All default targets; full CTest with `-j4` and native resource lock | 184/184 PASS,113.18s | `build/build-final-gcc.log`, `build/test-final-gcc.log` |
| Clang19.1.7, default, Werror | Fresh all default targets; full CTest with `-j4` | 184/184 PASS,100.98s | `build-clang/build-final.log`, `build-clang/test-final.log` |
| Clang19.1.7, Release, ASan+UBSan+LSan, Werror | Fresh all default targets, then affected-target rebuild after cancellation corrections; serial scoped27 | 27/27 PASS,95.50s | `build-asan/build-final.log`, `build-asan/build-cancel-admission.log`, `build-asan/test-scoped-final.log` |
| Same sanitizer profile, final03/09 admission barriers | Rebuild affected targets;03cancel and09messaging repeated five times each | All10 executions PASS,35.16s | `build-asan/build-admission-barriers.log`, `build-asan/test-admission-barriers-repeat.log` |

The two full compiler suites precede the cancellation-only test corrections
`af6048b`/`ae199d4`; the27-case sanitizer result includes them and precedes the
final03/09 acknowledgement barriers. Their final affected repetitions are
reported separately above. No production code changed.
Independent review must run its own fresh final-head package checks;
these earlier184 results are not represented as final-head full-suite results.
The final scoped27 comprise all11 O9 modes, six unchanged O4 native modes, three
shared support controls and seven mapped component targets.

All builds use `-DCOG_ENABLE_WERROR=ON` and `--parallel 2`; the separate sanitizer
directory additionally uses `-DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Release
-DCOG_ENABLE_ASAN=ON`. Test environment is
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. The real native executable's
flags include `-fsanitize=address,undefined`. `build-asan/native-ldd.log` records
the admitted external curl/c-ares paths; external dependencies are uninstrumented.
Deliberately SIGKILLed workers cannot supply an exit-time leak report. No leak
claim is made for those killed processes or third-party code.

The dependency arguments are the accepted checkout-local
`integrate-headless/build-dependencies/cmake-args.txt` profile. Shared fixture
Release flags independently show `-UNDEBUG` after `-DNDEBUG`. Original six
component bodies and both original O4 native fixture files remain preserved;
the entire production-library/application-source/header diff from `e8c155f`
is empty. Independent support correction review in ticket `4b5a102` also passed
an additional actual SIGTERM-to-supervisor descendant-cleanup control.

The initial scoped sanitizer run passed25/27 and exposed two test assumptions:
03 treated provider entry as if the coordinator had already observed RUNNING;
10 expected CANCELLED even though a native child had already factually refused
admission with FAILED/AGAIN before cancellation was requested. A syscall-only
diagnostic proved contention on the actual store `.cog-domain.lock` between the
two native processes. This is the existing documented nonblocking constructor
refusal, not permission to retry a consumed admission. The retained diagnostic
used ptrace and triggered its known LSan incompatibility; it supplies lock-origin
evidence only and is excluded from sanitizer acceptance. The test-only startup
synchronization above preserves that production behavior. Both corrected
cancellation controls then passed with their exact five TLS requests under
ASan/UBSan/LSan before the full scoped rerun.

Independent final-head Release review then exposed two further ordering
assumptions (182/184 at `a76326a`).03 registry RUNNING could precede the separate
slot START acknowledgement; all required facts now gate cancellation rather
than being asserted after a partial barrier.09 send1 returned factual
INVALID_STATE while sends2..9 succeeded because IDs were published before the
receiver slot was STARTED. Publication now waits for both actual slots, with
every whole-message/pressure/empty-poll/authorization assertion retained. No
operation budget was increased. The final affected sanitizer repetitions passed;
independent optimized repetitions and fresh package review remain pending.
