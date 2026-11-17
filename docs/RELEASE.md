# Release validation and assertion map

Whole O is independently accepted and merged on main. Supported-profile release
validation passed; this manifest records the evidence and `77adfec` tracks acceptance.
This is a documentation-only record: no new runtime/model request was made.

## Exact identities

| Record | Identity / evidence |
|---|---|
| Independently tested whole-O code | `5df406946c4ba5bc5203183e4af147b867f9cc5b` |
| Independent package verdict | `5cd1d25` PASS; complete profile record (`tix show 5cd1d25`) at `87e852fc0dba380344c711705eeb67057621cafd` |
| Approved integration merge | `b9140b68b689c0f70cedb5318347d5513f8bef79` |
| Accepted main merge | `c052b1722e42d9a8f42fc79bf35f29e4114e32a7` |
| Identical accepted integration/main tree | `e5ca810b2eaf8d86abd1dfc36a94deac459637d4` |
| Independent whole-O machine receipt | `whole-o-review-receipt.json`, SHA-256 `2bbe288476b4cd79f90a89c847a0ff09b6dc6fea3d0faee41ee3c70bb5cd1c36`; retained path and per-profile cache/log/inventory/install hashes are in the profile record (`tix show 5cd1d25`) |
| Coverage | [39 concrete assertion rows](RELEASE.md#assertion-map): original 20,05b and R01–R18, unchanged by this status refresh |
| Runnable guide review | `bf299a9` PASS for `299e661`; command validation (`tix show 71bcd03`) retains mock-only execution and source-audit boundaries |
| Installed public proof | `7eb47a1` PASS for `e77947c`, integrated by `f016189`; installed receipt (`tix show f016189`) |

These identities describe the accepted release snapshot, not a claim that every
subsequent checkout has been retested. Documentation consolidation adds no new
runtime evidence. Full historical receipts are preserved in the cited tickets.
Machine-readable receipts retain their original bytes and historical document
paths; `tix show bcc629b` resolves those paths to the archived source records.

## Independent supported-profile results

All producer builds use Release, strict C11 warnings/Werror and at most two
build jobs. The full three profiles have no exclusions, disabled tests, skips
or failures. Exact commands, log hashes and actual conditional-registration
inventory are retained in the independent receipt above.

| Profile | Actual result | Seconds |
|---|---:|---:|
| GCC14.2.0 Release | 185/185 | 95.54 |
| Clang19.1.7 Release | 185/185 | 94.51 |
| Clang19.1.7 Release ASan/UBSan/LSan | 185/185 | 99.85 |
| Separate GCC14.2.0 Release TSan | 36/36 selected | 150.08 |
| Public headers: GCC/Clang C11 and G++/Clang++ C++17 | 28 headers ×4 compilers = 112 checks | — |

The TSan selection covers stop/cooperative core ownership, registry/config,
toolkit, IPC/orchestrator, child cancel/crash/mailboxes/fanout, reconnect,
migration/fencing, process-owner controls and the installed consumer. It is not
185 TSan tests. All 58 production compilation units are instrumented in the
corresponding sanitizer build. External curl/c-ares/system libraries are not;
deliberately killed processes cannot supply an exit-time leak report.

Normal full installs and two relocations pass in each profile. Both public
C11 consumers, CLI-only transitive orchestrator closure, custom fenced
release/preallocation negatives, actual relocated native READY/PID/UID/inode,
reap/retirement, two-request REAL app/tool/durable result and separate installed
MOCK headless/attach/watch roles are exercised. Positive process proof reports
cleanup true with zero infrastructure child kills. No private host-runtime
header or trust bypass is part of installed embedding.

## Admitted dependency and live evidence

| Artifact | SHA-256 |
|---|---|
| curl | `ae1a9e77488254d744c9064972b025d8eb7210e73ea900cd5f3632995bb29d18` |
| c-ares | `b4d2377241c646dc2054e864e403f162d4b5969823bdd86ec5ff49ecf7e79f53` |
| [Sanitized M live receipt](REAL_CLIENT_M_LIVE_RECEIPT.json) | `187516695653a1efaacfaa12aa76c93a41def0c7eb6b0bd410f2f5b0ce1e8d12` |

The accepted profile is Linux/POSIX with c-ares as primary resolver,
`ENABLE_ARES=ON`, `ENABLE_THREADED_RESOLVER=OFF`, fresh HTTP/1.1, no connection
pooling/HTTP2 and application-only retry ownership. Installed metadata and
actual loader paths use the relocated admitted objects without manual staging
or loader overrides. OpenSSL/libc/zlib/zstd/CA trust remain host dependencies.
Third-party dynamic memory is declared; no total dependency-memory cap exists.
These tested artifacts/profile results are not a universal platform guarantee.

M's dated live application record (`tix show 80e782e`) remains
exactly three generations: two initial read/edit/build requests and one
fresh-process resume. It uses the installed public app, not a literal CLI
hard-cap proof or a remote-child paid/live run. No additional paid call was made
for whole-O review or this manifest; local REAL TLS scripts remain offline
protocol/effect evidence.

## Finite behavior boundaries

The [assertion map](RELEASE.md#assertion-map) distinguishes canonical
same-group/retired sibling IDs from component live foreign-group refusal.
Test-only constructor admission staging keeps original frames/deadlines and
still requires all three actual children active together; an allowed
pre-STARTED AGAIN is not a production retry promise. Admitted worker failure
retains INDETERMINATE and does not authorize automatic replay/replacement or
external-effect rollback. Migration is same-host exact-checkpoint transfer,
not cross-host/live-handle migration. Installed native lifecycle and separate
public REAL app do not claim installed remote-worker REAL HTTP or composed
handoff; those are actual O9 in-tree joins. Standalone C++ header conformance
does not turn the C11 installed runtime hosts into a C++ runtime proof.

All current builds/logs/TLS scratch stay in checkout-local `build*` directories.
Dated M/F/H/O evidence, including older counts/paths/failures, remains intact.

## Assertion map

This map joins the twenty original scenarios, shutdown regression 05b and
R01–R18: 39 assertion rows, not 39 independently named CTests. It describes the
accepted main `c052b172`, with whole-O code `5df4069` independently approved by
`5cd1d25`. Supported-profile release validation passed; `77adfec` tracks
acceptance. A target name or design
trace alone is not runtime evidence. See the
[current release manifest](RELEASE.md) for exact identities.

## Evidence and profile boundaries

| Record | Established scope |
|---|---|
| M offline (`tix show d8bddea`) and [manifest](REAL_CLIENT_M_TEST_MANIFEST.json) | Dated single-process GCC/Clang Release and ASan/UBSan/LSan 129 applicable cases, separate 13-case TSan, maintained headers and installed consumer. Its six O exceptions remain visible in that historical record. |
| M bounded live app (`tix show 80e782e`) | Exactly three real generation requests: installed public app read/edit/build, then fresh-process resume. It is not a literal CLI paid-request-cap proof or a remote-child live-model run. |
| Focused M joins (`tix show 83b30aa`) | Actual ASK/output faults, shared retry/tool budgets, real transport cancellation phases, completed-effect/opaque fork and terminal/signal ownership assertions. |
| O9 case map (`tix show f63652b`) and accepted integration (`tix show 8537bc9`) | Actual REAL loopback/native replacements for 03/07/09/10/11/17, five extra modes and preserved components. Postmerge strict GCC Release 184/184; exact separate compiler/sanitizer/repetition scopes remain in those receipts. |
| Installed public integration (`tix show f016189`) | Accepted integrated GCC Release 185/185 with no disabled/skipped/excluded cases. Actual normal installs/two relocations, public-only CLI closure, owned native lifecycle, custom fenced backend, separate REAL TLS app and installed MOCK UI roles. |
| Whole-O independent review (`tix show 5cd1d25`) | Exact code `5df4069`: full GCC 185/185, Clang 185/185 and ASan/UBSan/LSan 185/185; separate meaningful TSan 36/36; all 28 public headers in 112 strict C11/C++17 combinations. Final installed proof independently passes in each profile. |

The 185 count is conditional on the admitted curl/c-ares, CLI/Linux and installed
fixture configuration. Real HTTPS/OOM/net cases require
`COG_NET_CURL_BUILD_DIR` and `COG_NET_CURL_LIBRARY`; installed proof additionally
requires `COG_TEST_CARES_BUILD_DIR`. An unsupported-profile `unit-net` refusal
does not substitute for these tests. The current full suite does not retain M's
six exceptions. Whole-O supported-profile validation is accepted; its fresh
results are distinct from older M scopes. The release manifest records
supported-profile validation; `77adfec` tracks acceptance.

## Original 01–20 and 05b

Canonical stems below have the CTest prefix `scenario-`. Source01–10/05b live in
`tests/scenarios`;11–20 live in `applications/cog-cli/tests`. Supplementary names
are exact registered CTests. Each row names the observable assertion, with M/O9/
installed scope supplied above.

| ID | Canonical / supplementary executable cases | Actual oracle |
|---|---|---|
| 01 | [test_01_user_prompt](../tests/scenarios/test_01_user_prompt.c); `cli-app-https` | Mock stream forms one complete aggregate assistant, exact two-node history and one send. Separate REAL TLS case proves typed tool/opaque continuation and durable result rather than pretending the mock is a live provider. |
| 02 | [test_02_network_recovery](../tests/scenarios/test_02_network_recovery.c); `cli-client-acceptance`, `cli-client-wire-budget` | Recovery history/two sends; exact retry/turn equality refusal and one-inside admission. Actual HTTP429/tool continuation retains the original whole-turn deadline. |
| 03 | [test_03_subagent_lifecycle](../tests/scenarios/test_03_subagent_lifecycle.c), `-cancel` | Real local/native provider/tool attempts, factual registry RUNNING plus independent STARTED admission, parent/child durable linkage, terminal semantic ACK, saved parent TOOL/ledger, retirement and fresh IDs on repeated groups. Cancellation requires the real held request and every admission fact. |
| 04 | [test_04_cold_start](../tests/scenarios/test_04_cold_start.c); `cli-configuration-memory`, `cli-app-memory`, `cli-empty-session` | Component initialization/zero premature sends and first exchange; constructor fault unwind and initial durable empty checkpoint. |
| 05 | [test_05_graceful_shutdown](../tests/scenarios/test_05_graceful_shutdown.c); `cli-app-stop`, `cli-app-limits`, `cli-app-save` | Idle/in-flight shutdown, retained cancelled conversation, bounded provider/tool drain and durable failure behavior. Remote cleanup is additionally joined in03/10/O4 native modes. |
| 05b | [test_05b_shutdown_cancel_contract](../tests/scenarios/test_05b_shutdown_cancel_contract.c) | Submit then shutdown still admits the one actual in-flight send, requests cancellation, preserves exact user bytes/CANCELLED assistant and returns IDLE; never-started shutdown remains idle. |
| 06 | [test_06_parallel_tools](../tests/scenarios/test_06_parallel_tools.c); `unit-toolkit-parallel`, `unit-toolkit-async`, `unit-workspace` | Actual overlap, submission-correlated results, timeout/cancel, result events, retained buffers and owned process cleanup. Normal app tool execution remains sequential. |
| 07 | [test_07_aux_crash](../tests/scenarios/test_07_aux_crash.c), `-crash-after` | Kill only an actual owned unreaped native worker after admitted RUNNING, before/after an observed effect. Both preserve first error/INDETERMINATE and durable uncertainty, reap, and refuse ordinary REAL resume with no extra request/effect. No automatic failover claim. |
| 08 | [test_08_config_reload](../tests/scenarios/test_08_config_reload.c); `cli-app-reload`, `cli-app-reload-critic` | Candidate validation/rollback plus actual app generation pinning across request/retry/tool/ASK, transactional publication, provider borrow/retirement and EOF boundaries. |
| 09 | [test_09_subagent_messaging](../tests/scenarios/test_09_subagent_messaging.c); `component-coordinator-admission` | Actual sibling tools use observed IDs only after both executor slots are STARTED; whole payload/source IDs, empty polls and two groups. Full eight-entry mailbox retains the ninth send/deadline until progress. Canonical refusal uses a retired ID; live foreign-group refusal is the separate real mailbox component. |
| 10 | [test_10_fan_out_fan_in](../tests/scenarios/test_10_fan_out_fan_in.c), `-fanout-cancel`, `-fanout-limit` | Three actual children on local plus two distinct authenticated native peers; staggered terminal observations, submission-ordered results, once-only effects/reuse. Cancellation sees all three active; limit refuses before STARTED. Constructor lock contention can legitimately refuse admission; no product retry is inferred. |
| 11 | [test_11_ui_reconnect](../applications/cog-cli/tests/test_11_ui_reconnect.c) | Physical sockets and retained actual client state; replay-ring GAP and atomic replacement, stable identity, pending ASK with no implicit ALLOW, genuinely lost final/two identical command copies with one effect, expired original frame cannot regain control. |
| 12 | [test_12_rate_limit](../applications/cog-cli/tests/test_12_rate_limit.c); `cli-client-acceptance`, `cli-client-wire-budget` | Retry metadata, bounded backoff/queued order/send counts, shared elapsed/deadline equality and actual HTTP429. |
| 13 | [test_13_session_persistence](../applications/cog-cli/tests/test_13_session_persistence.c); `unit-store-fencing`, `unit-store-legacy-admission`, `cli-installed-session-ux`, `cli-installed-empty-session` | Exact structured message/result round trips; corruption, immutable checkpoint/CAS/crash boundaries; fresh-process restart, empty checkpoint and guard release. |
| 14 | [test_14_conversation_fork](../applications/cog-cli/tests/test_14_conversation_fork.c); `cli-client-wire-fork` | Shared immutable ancestors/distinct histories/costs; actual outgoing TLS history retains exact opaque/result identity, reused textual call IDs get new assistant scope, old effect/leaf unchanged. |
| 15 | [test_15_multi_turn_tools](../applications/cog-cli/tests/test_15_multi_turn_tools.c); `cli-client-acceptance`, `cli-client-ask-output-initial`, `cli-client-ask-output-repeat` | Actual durable async `tool_round` ALLOW/DENY/no-store, correlated result/ledger/audit before continuation. Arbitrary input is not ASK authorization; undeliverable prompt admits no STARTED/effect. |
| 16 | [test_16_error_propagation](../applications/cog-cli/tests/test_16_error_propagation.c); `cli-app-loop`, `cli-app-save`, `unit-provider-preflight-critic` | Retriable/hard send counts and symbolic diagnostics; partial unsafe history and retained effect result survive failure, no automatic replay. |
| 17 | [test_17_session_migration](../applications/cog-cli/tests/test_17_session_migration.c), `-timeout` | Four actual local/native moves with exact checkpoint/history/owner/guard/route order, a new real effect after each activation, dropped genuine cleanup reply/retained receipt and slot reuse. Post-release/CAS SIGSTOP keeps source frozen without route/activation and reaps. Same-host checkpoint transfer only. |
| 18 | [test_18_observability](../applications/cog-cli/tests/test_18_observability.c); `cli-observation-normal`, `cli-observation-retry` | Admitted profile invokes actual TLS/tool observation bridge, not only manually created spans. Provider/tool duration/status, known/unknown usage, generation labels and secret canaries. |
| 19 | [test_19_connection_events](../applications/cog-cli/tests/test_19_connection_events.c); `cli-observation-critic`, `unit-net-observer-critic` | Real OPEN/CONNECTED/PROGRESS/terminal chronology, exact cumulative/delta counts, one terminal, actual retry and sanitized labels; retained IDs/foreign requests and consuming target detach. |
| 20 | [test_20_interrupt_and_queue](../applications/cog-cli/tests/test_20_interrupt_and_queue.c); `cli-client-wire-cancel-finish`, `cli-client-wire-budget` | Retained CANCELLED assistant and FIFO input, fresh continuation after failure; actual retained FINISH wins cancellation and queued traffic does not extend deadlines or trigger replay. |

The six replaced originals retain their old assertions in
`component-agent_lifecycle`, `component-aux_crash`,
`component-subagent_messaging`, `component-fan_out_fan_in`,
`component-ui_buffer` and `component-store_transfer`. See the
source mapping (`tix show f63652b`).
These components do not substitute for the canonical REAL joins.

Where a positive/cancellation fixture stages constructor admission, it delays
only an unaccepted actual transport copy and retains the original request and
deadline. It must still observe all three actual children active together on
the distinct placements. A legal pre-STARTED store-lock AGAIN remains a factual
admission refusal; fixture scheduling is not a production retry guarantee.

## R01–R18

| ID | Exact executable cases / source | Actual oracle |
|---|---|---|
| R01 | `cli-configuration`, `cli-configuration-memory`, `cli-app-memory`; [configuration tests](../applications/cog-cli/tests/test_configuration.c) | Precedence/presence/explicit zero, copied credentials/config generation, failure/redaction/session provenance and constructor unwind. |
| R02 | `cli-app-https`; [wire driver](../applications/cog-cli/tests/run_app_https.py); accepted M live app | Local REAL TLS: two requests, actual edit, four-message durable round, exact opaque/call/result and usage. M's separate three-request live coding/resume outcome is dated and not rerun here. |
| R03 | `unit-openrouter`, `unit-provider-preflight-critic`; [codec cases](../tests/unit/test_openrouter.c) | Fragmented JSON/SSE/UTF-8/tool identities, retained next/capacity, terminal/opaque bytes, bounded queue/depth/OOM and preflight state atomicity. |
| R04 | `cli-app-loop`, `cli-app-save`, `cli-client-acceptance`, `cli-client-wire-budget`, `unit-store-fencing` | Durable STARTED before effect; correlated result plus terminal ledger/audit before next send. Save/syscall/crash faults freeze unsafe work without rerun; real tool marker and next-send checkpoint inspection. |
| R05 | `cli-client-acceptance`, `cli-client-ask-output-initial`, `cli-client-ask-output-repeat`, `cli-app-limits`, `unit-workspace` | Interactive/noninteractive ASK, ALLOW/DENY/no-store, arbitrary input not authorization, failed prompt no effect, schema/path/traversal/symlink refusal. |
| R06 | `cli-client-acceptance`, `cli-client-wire-budget`, `cli-app-stop` | Exact elapsed0/5 equality refusal and one-inside admission, min retry/turn deadline, real429/tool continuation shares original start, backoff stop. |
| R07 | `cli-app-loop`, `cli-session-ux`, `unit-openrouter`, `unit-net-error-cancel-regression` | One incomplete aggregate/no replay, unsafe restored branch/queued input not fresh authorization, accepted-output transport safety and explicit continuation. |
| R08 | `cli-client-wire-cancel-dns`, `cli-client-wire-cancel-connect`, `cli-client-wire-cancel-tls`, `cli-client-wire-cancel-stream`, `cli-client-wire-cancel-retained`, `cli-client-wire-cancel-finish`; [wire cancellation driver](../applications/cog-cli/tests/run_client_wire_acceptance.py) | Actual DNS packet/SYN_SENT/TLS ClientHello/text/one-slot retained delta/retained successful FINISH before cancellation. Provider inactive, net slots zero, thread/FD baseline restored and guard reacquired. |
| R09 | `unit-workspace`, `unit-toolkit-async`, `unit-toolkit-resources`, `unit-toolkit-fencing`, `unit-toolkit-review-faults`, `cli-app-limits` | Owned command/group readiness, TERM/KILL deadlines, bounded retained/truncated output, descendant reap and no command credential inheritance. |
| R10 | `cli-session-ux`, `cli-session-recovery-faults`, `cli-installed-session-ux`, `cli-empty-session`, `cli-installed-empty-session`, `unit-store-legacy-admission` | Explicit restart/list/resume/recover selectors, failed reconciliation save freezes, fresh/interrupted initial checkpoint, exact v1 migration, six-name ambiguous legacy refusal and committed-v2 precedence. |
| R11 | `cli-client-wire-fork`, `unit-session` | Completed-effect/opaque ancestor exact outgoing bytes and identified result, preserved old leaf/effect, independent new assistant ledger scope, immutable shared prefix. |
| R12 | `cli-app-reload`, `cli-app-reload-critic`, `cli-configuration-clone` | Actual provider generation prepare/publish/retire, whole-turn pinning across request/retry/tool/ASK, rollback, finite attempts/capacity, consuming disconnect, restored-proposal preflight/notice/EOF. |
| R13 | `scenario-test_11_ui_reconnect`, `cli-view-client`, `cli-view-projection`, `unit-ipc-replay`, `unit-ipc-replay-critic` | Actual coordinator reconnect/GAP/full replacement/ASK/retained decision and no repeated effect, alongside codec/cursor/retention primitives. Read-only client cannot admit commands. |
| R14 | All03/07/09/10 canonical modes; `unit-agent`, `unit-orchestrator-wire`, `component-coordinator-admission` | Authenticated actual native attempts/updates/ACK/result, exact owned crash/reap, sibling tools, distributed three-child group and cancellation, no replay. Same-group/retired-ID and component live foreign-group scopes remain distinct. |
| R15 | `unit-openrouter`, `unit-provider-preflight-critic`, `unit-net-oom`, `unit-net-cog-oom`, `unit-net-https`, `cli-app-save`, build assertion/allocator controls | Malformed/deep/oversized/opaque/compatibility input fails atomically; positive allocation interception remains active under Release, retained results and secret-safe diagnostics. |
| R16 | Original05/05b; `cli-app-stop`, `cli-app-limits`, `cli-client-installed-output`, `cli-installed-session-ux`;03/10 cancel and unchanged O4 native modes | Local/backoff/tool/real-phase/output/signal drain, empty durable restart and recovery authority. Native attempts cancel/drain, owner remains until actual reap/close; uncertainty never authorizes replay. |
| R17 | `cli-client-acceptance`, `cli-client-wire-budget`, `cli-app-limits`;10 `fanout-limit` | Exact cumulative schema/history/call/result/opaque context bytes, equality/one-over before send, semantic LENGTH/CONTENT_FILTER no tools, retry/tool/turn/output/round caps and whole-result admission refusal. |
| R18 | `integration-installed-embedder`, `unit-installed-path-audit`, `unit-installed-process-owner`; [external public project](../tests/external-consumer/README.md) | Normal full install/two relocations, nine-callback provider, two independent custom store/net contexts and reverse teardown, host curl lifetime, CLI-only export closure, actual installed worker/roles, custom fenced ownership/OOM, separate REAL TLS app, admitted bytes and loader/metadata path negatives. |

For R18 the installed public host does not include private host-runtime headers
or claim installed remote-worker REAL HTTP/composed handoff. Those are O9's
in-tree actual joins. The CLI trust template has no fixture-local CA input.
For07/R14, a no-effect marker before worker death still leaves an admitted task
INDETERMINATE. Migration is same-host exact-checkpoint transfer; no cross-host,
live handle transfer, automatic retry/replacement or external-effect rollback is
claimed. Loopback REAL scripts are not paid/live model behavior.

## Final gate record

The accepted whole-O review froze code and admitted dependency provenance,
checked the actual enabled inventory, ran the full supported GCC/Clang/ASan
suites without old M exceptions, and retained exact TSan/process-stress scopes.
It verified C11/C++ headers, assertion/allocator controls, normal installed
closure and these unchanged 39 assertion rows. Main `c052b172` preserves the
approved integration tree. Supported-profile release validation passed;
`77adfec` tracks acceptance.
The [release manifest](RELEASE.md) indexes exact hashes and
limits. Runnable commands are in [README.md](README.md); dated M/F/H/O receipts
and old paths/counts remain historical evidence rather than being edited.
