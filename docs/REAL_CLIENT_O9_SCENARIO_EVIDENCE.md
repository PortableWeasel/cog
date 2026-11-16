# O9 joined scenario evidence

O9 replaces six historical canonical scenario substitutes with actual bounded
headless runtime joins. The production tree is unchanged from the accepted O4
baseline `e8c155f`. Original assertions remain separately selectable component
tests. Independent O9 package review and integration remain separate gates.

The assembled source snapshots are native `a32b0c8`, sessions `956fa26`, shared
support `fc97ce4`, and the sole-owner registration amendments. Source history in
the assembled branch preserves each test-only checkpoint; no passing result is
inferred from its name, a scripted model reply, or a manually completed agent.

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

The final assembled compiler/sanitizer matrix and independent review references
are recorded here after their actual runs. Passing leaf checkpoints establish
their own cases only; they do not replace final assembled-package validation.
