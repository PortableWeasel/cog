# M independent offline acceptance

Ticket `d8bddea`, under single-process gate `d0f910f`. The initial immutable
candidate is `1329018c230c105f4c0f6b917f50e8a3fd0aac03`. This record covers the
compiler, sanitizer, installed-consumer and assertion audit. The bounded paid
application smoke has a separate owner (`80e782e`); no credentials or paid
requests were used here. M is not release gate P, and this review cannot close
N/O runtime obligations by counting old scenario target names.

Final reviewed candidate includes the test/docs-only header amendment
`017c0b48942a918133c4605573264176b43f9de3`; production sources are unchanged
from the initial candidate. Independent offline review **passes with the six
explicit O exceptions**. The parent M decision still requires its separate live
application evidence and belongs to the orchestrator.
Integrated main `5eeb96eb4eccc629e633a75971bff09d9f3a06ca` has an identical
tree to that reviewed candidate; the no-ff merge changed no files.

## Profile and commands

The isolated managed checkout is `gate-single-process`, branch
`codex/gate-single-process`. Separate `/tmp/cog-m-gate-{gcc,clang,asan,tsan}`
builds use the accepted final installed dependency artifact, not system curl:

Host tools: GCC 14.2.0, Clang 19.1.7, CMake 3.31.6, Linux
6.12.96+deb13-amd64. Actual Release compile commands retain `-O3 -DNDEBUG` on
production and append `-UNDEBUG` only for test assertions.

```sh
cmake -S . -B BUILD -DCMAKE_BUILD_TYPE=Release -DCOG_ENABLE_WERROR=ON \
  -DCMAKE_C_COMPILER=gcc \
  -DCURL_LIBRARY=/tmp/cog-relocatable-profile/shipped/curl-build/install/lib/libcurl.so \
  -DCOG_NET_CURL_LIBRARY=/tmp/cog-relocatable-profile/shipped/curl-build/install/lib/libcurl.so \
  -DCOG_NET_CURL_BUILD_DIR=/tmp/cog-relocatable-profile/shipped/curl-build \
  -DCURL_INCLUDE_DIR=/tmp/cog-relocatable-profile/shipped/curl-build/install/include \
  -DCOG_TEST_CARES_BUILD_DIR=/tmp/cog-relocatable-profile/shipped/cares-build
cmake --build BUILD -j4
ctest --test-dir BUILD --output-on-failure -j4
```

Clang uses `-DCMAKE_C_COMPILER=clang`; the separate Clang sanitizer build adds
`-DCOG_ENABLE_ASAN=ON`, with `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. Separate GCC TSan uses Debug,
`-DCOG_ENABLE_TSAN=ON` and `TSAN_OPTIONS=halt_on_error=1`. Sanitizers are never
combined in one build. Project strict warnings and test-only assertion policy
remain enabled; no suppression or production source edit was made by this gate.

The frozen installed curl SHA256 is
`ae1a9e77488254d744c9064972b025d8eb7210e73ea900cd5f3632995bb29d18`;
c-ares is `b4d2377241c646dc2054e864e403f162d4b5969823bdd86ec5ff49ecf7e79f53`.
The original artifacts retain these hashes after installation checks. Normal
install bookkeeping is distinct from immutable library bytes. System OpenSSL,
libc, zlib/zstd and CA trust remain the explicitly accepted host dependencies;
this is not a claim of a self-contained operating-system bundle.

## Exact runtime boundary

Initial configurations omitted only the optional installed-consumer prerequisite
`COG_TEST_CARES_BUILD_DIR`. Each full strict GCC, strict Clang and ASan/UBSan/LSan
run executed all **133** registered tests: **127 passed and the same six failed**.
This is not a full-suite pass. Enabling the admitted c-ares build registers R18's
permanent installed-consumer test; its outcome is recorded separately below.

| O exception | Observed failure and verified source reason |
|---|---|
| `unit-orchestrator` | `add_self_core` assumes connect returns OK; actual AGAIN is -8. Subsequent missing peer/session assertions cascade. Strict runs segfault; UBSan pinpoints null argument to strcmp at `tests/unit/test_orchestrator.c:662` after failed setup. |
| `integration-orchestrator-ipc` | No healthy registration/session, placement and stop-timeout assertions fail. Aux fixtures assume synchronous connect and orchestration does not service nonblocking endpoint progress. |
| `integration-orchestrator-stress` | Connect gets AGAIN instead of expected OK; health/replacement/stop assertions fail for the same missing service integration. |
| `scenario-test_07_aux_crash` | Expected session registration and healthy/replacement peer never become ready. |
| `scenario-test_09_subagent_messaging` | Zero registrations/routes, no hello/pong; explicit connect/recv expectations still require immediate OK. |
| `scenario-test_10_fan_out_fan_in` | Health/placement and literal cancellation broadcasts do not complete. |

Source inspection confirms `libraries/cog-orchestrator/src/orchestrator.c`
still uses send/recv/poll without `cog_ipc_endpoint_service`. Ticket `d5280b7`
owns both production progression and fixture migration. Changing only a helper
assertion would not establish the remote protocol. No additional registered
runtime failure occurred in the initial three full runs. No test process from
these builds remained running afterward.

The only permitted M exclusion expression is:

```text
^(unit-orchestrator|integration-orchestrator-ipc|integration-orchestrator-stress|scenario-test_07_aux_crash|scenario-test_09_subagent_messaging|scenario-test_10_fan_out_fan_in)$
```

Separate TSan covered **13/13** cases: unit-core, unit-core-service, unit-config,
unit-config-bounded, unit-toolkit-critic, unit-toolkit-parallel,
unit-toolkit-resources, integration-core-toolkit, scenario-test_06_parallel_tools,
cli-app-stop, cli-app-limits, cli-app-reload and cli-app-reload-critic. These cover
cross-thread stop publication, synchronized toolkit workers/barriers and creation
failures, process cancellation/drain, immutable config views and reload/retirement.
It is not a claim of arbitrary concurrent access to single-thread-owned APIs.

## Original scenario assertions

Source paths below are relative to the repository. Scenario 01–10 files reside
in `tests/scenarios`; 11–20 in `applications/cog-cli/tests`. A passing historical
fixture is not substituted for a missing remote/headless composition.

| Scenario | Concrete current assertion and M/P scope |
|---|---|
| 01 prompt | `test_01_user_prompt` checks streamed output, one aggregate assistant text `The answer is 42.`, exact two-node history and one send. `cli-app-https` adds actual TLS/tool/opaque composition. |
| 02 network recovery | `test_02_network_recovery` checks two sends and recovered history. `cli-client-acceptance` tests remaining retry/turn budget equality and one-inside; wire-budget joins actual 429, tool and original deadline. |
| 03 subagent | Existing scenario manually drives local agent steps; `unit-agent` proves lease/retention/codec primitives. Actual authenticated remote execution/acknowledgment remains O. |
| 04 cold start | `test_04_cold_start` checks initialized component pointers, zero sends before run, first user/assistant and one send; configuration/memory/HTTPS tests cover real constructor failures. Empty-session fixtures add initial durable checkpoint. |
| 05 and 05b shutdown | Exact CANCELLED assistant, preserved initial input, one send, idle/pending shutdown; app stop/limits/save and installed output/empty-session fixtures join drain, durable state and signals. Remote children remain O. |
| 06 parallel tools | Overlap, ordered correlated results, timeout, cancel and tool-result event cases; async toolkit/workspace tests supply retained results, group cleanup and pre-effect guard/policy checks. App execution intentionally remains sequential. |
| 07 aux crash | Failing O exception; no M end-to-end crash/failover claim. |
| 08 reload | Historical transaction snapshot/validation/rollback tests are supplemented by `cli-app-reload`: retained model, retry, tool and ASK turns pin old generation; failure preserves publication, retirement consumes disconnect, EOF does not activate unused future state. Critic cases check provider borrows. |
| 09 cross-core message | Failing O exception; no M authenticated routing claim. |
| 10 fan-out/fan-in | Failing O exception; no M remote placement or result-race claim. |
| 11 reconnect | Passing old local drop-oldest fixture is synthetic. CGR1 replay units and CGD1 codec cover primitives; actual detach/GAP/snapshot/dedup remains O. |
| 12 rate limit | Metadata, bounded backoff and queued order/send counts, supplemented by exact elapsed/shared-turn and actual HTTP429 oracles. |
| 13 persistence | Exact message and identified tool-result round trips; store codec/fencing validate immutable generations, corruption and crash publication; installed session UX/empty-session fixtures validate fresh-process restart and guard release. |
| 14 fork | Shared immutable prefix and distinct branch histories/costs; `cli-client-wire-fork` adds completed tool + exact opaque ancestor, reused textual call ID in a new assistant scope, old leaf preserved and no ancestor effect replay. |
| 15 tool rounds | Scenario invokes actual `tool_round` for ALLOW, DENY and unavailable durable store. Exact marker, identified result, ledger/audit and continuation assertions; ASK delivery failures forbid STARTED/effect. |
| 16 provider errors | Retriable versus hard-error send counts and symbolic stderr; app partial/save/preflight tests preserve unsafe branches/retained outcomes and forbid replay. |
| 17 migration | Passing baseline save/load-copy fixture is not CGH1 handoff. Exact checkpoint/CAS/guard primitives pass; quiescence, route publication and remote handoff remain O. |
| 18 observability | Admitted profile invokes `cog_test_observation_runtime` through actual TLS/tool fixture, alongside typed obs units. Provider/tool duration/status, known/unknown usage, secret canaries and real generation labels are asserted. |
| 19 connection events | Same real fixture asserts chronological OPEN/CONNECTED/PROGRESS/terminal rows, exact cumulative/delta counts, one terminal, actual retry only, sanitized labels and teardown; observer critic checks retained IDs, foreign requests and consuming target detach. |
| 20 interrupt/queue | Retained CANCELLED assistant, FIFO queue, explicit fresh continuation after failed turn; wire cancellation checks actual phase/retained FINISH and no provider retry. |

## R01–R18 assertion map

The [focused acceptance map](REAL_CLIENT_ACCEPTANCE_EVIDENCE.md) supplies exact
join details; the table below ties every revised scenario to currently executed
or explicitly deferred evidence rather than assuming an R-named target exists.

| Scenario | Concrete fixture/case/oracle |
|---|---|
| R01 config/startup | `cli-configuration` precedence/field matrix/failure/redaction/session configuration and `cli-configuration-memory`; copied credential/generation ownership, explicit zero and constructor unwind. No live credential used in this gate. |
| R02 real turn | `cli-app-https` + `run_app_https.py`: actual local TLS provider, two requests, one real edit, four-node durable round, exact opaque/result association and usage. Paid model behavior is separate M smoke. |
| R03 fragmented codec | `unit-openrouter` valid/overflow_cancel/request_copy_and_opaque/terminal_and_opaque/capacity/depth cases: fragmentation, UTF-8, identity, bounded queues, retained output and exact continuation. Preflight critic compares independent history grammar and state atomicity. |
| R04 effects | `cli-app-loop` tool_round, `cli-app-save`, `cli-client-acceptance` next-send durable checkpoint inspection, real wire tool and store-fencing syscall/crash faults. STARTED precedes execution; result+terminal ledger precede next model request. |
| R05 denied/invalid tools | ALLOW/DENY/no-store app cases, interactive/noninteractive ASK, arbitrary input not authorization, initial/repeated prompt output failure no STARTED/effect, schema/path/symlink/workspace refusal. |
| R06 retry | `cli-client-acceptance` retry_boundary at elapsed0/5 with exact equality refusal and one-inside admission; `cli-client-wire-budget` holds original start across HTTP429/retry/tool/continuation and busy queued traffic. |
| R07 partial failure | `cli-app-loop` retains one incomplete aggregate, no replay and fresh authorization; `cli-session-ux` restored unsafe branch/queued authority; OpenRouter transport_errors and net completion/error/cancel regressions enforce accepted-output replay safety. |
| R08 cancel | Wire modes observe actual DNS packet, SYN_SENT, TLS ClientHello, accepted text, one-slot retained delta and successful FINISH before cancelling. App/provider inactive, net slots zero, thread/FD baselines restored and store guard reacquired. Backoff cross-thread stop remains separately tested. |
| R09 process cleanup | `unit-workspace`, toolkit async/resources/fencing/review-faults and `cli-app-limits`: child/group readiness, TERM/KILL deadlines, retained truncated output, cancellation, descendant reap and no command credential inheritance. |
| R10 persistence/recovery | Session UX/installed UX/recovery-faults plus empty-session fresh/interrupted/save_failures/admission_failures/selectors_and_remnants/provenance cases. Legacy-admission matrix refuses ambiguous six-name remnants, exact v1 migration and committed-v2 precedence remain intact. |
| R11 fork | Actual TLS wire fork checks opaque bytes/result identity in outgoing history, unchanged old effect before new command, separate ledger assistant identity and preserved old leaf. |
| R12 reload | `cli-app-reload` and critic explicitly cover whole-turn generation pinning, transactional rollback, fixed attempts/capacity, failed consuming retirement, restored-proposal preflight, pending notice and EOF boundaries. |
| R13 UI replay | CGR1 replay/critic and CGD1 app-codec tests pass as primitives. Actual UI reconnect/GAP/full active snapshot composition remains O; old scenario11 is insufficient. |
| R14 remote child | Core agent/IPC/guard primitives pass. Actual authenticated CGA1 remote attempt, cancellation/crash/reap and parent outcome composition remains O. |
| R15 malformed input | OpenRouter failures/constructor_failures/depth/opaque-profile, independent preflight and net capacity/TLS/replay/OOM tests; positive allocator interception controls stay active under Release and Clang. |
| R16 shutdown | Original05/05b, app stop/limits/save, real cancellation phases, EPIPE, full stdout/stderr with masked signals, empty durable checkpoint restart and owner-only fenced recovery. Remote child shutdown remains O. |
| R17 budgets | Cumulative context includes schema/history/call/result/opaque exact bytes, equality and one-byte-over before-send decisions; LENGTH/CONTENT_FILTER forbid tools; shared retry+tool/turn deadline and output/tool-round limits. |
| R18 installed embedder | Permanent external C11 consumer uses normal installs and two relocations, all exported graphs, nine-callback provider, two independent custom stores and net contexts, reverse destruction, admitted SHA and loader/metadata closure. O orchestrator runtime is intentionally not established by symbol/basic lifecycle coverage. |

## Deviations and final receipts

The first ASan build encountered ENOSPC on the shared `/tmp` tmpfs before tests.
Only this agent's obsolete completed-review CMake build outputs were cleaned;
caches/logs, frozen dependencies, other active work and `build_user/` were
preserved. Resuming the same sanitizer build succeeded and ran all133 tests.
This infrastructure interruption is not a suppressed product failure.

A direct broad header audit confirmed all 28 public headers pass standalone under
both strict compilers (56/56), but found real drift in unregistered smoke sources:
a missing async/flags initializer and five missing final newlines. Ticket
`97ab2e0` supplies the bounded correction and maintained public-header registration.
Private vendor macro-only and codec headers are not installed standalone public
contracts; applying the obsolete blanket all-library-header command to them was
also diagnosed as an invalid test boundary, not repaired through warning
suppression. The reviewer accepted `017c0b4` after checking scope, registration,
exact source inventory and actual compile flags. All three fresh reconfigured
all-target builds pass. Each includes **44** strict C11/Werror/UNDEBUG header
translation units: **28** public includes plus **16** maintained type-use sources.

The final configurations contain **135** tests. Each compiler/sanitizer profile
passes **128/128** tests in the non-install applicable batch, followed by the
installed embedder **1/1**, giving **129/129 applicable tests**. R18 was rerun
serially on the final candidate after the header-only amendment, using normal
installs into separate disposable prefixes. Its reports confirm curl 8.23.0,
c-ares 1.34.9, nine-callback provider/custom-store/net lifecycle and admitted
hashes. The only six remaining registered cases are the previously observed O
exceptions; there are no additional skipped or failed applicable tests.

The batch command adds `|integration-installed-embedder` to the exact exclusion
expression above solely to separate shared dependency install bookkeeping from
parallel compiler runs. It is then executed independently, not excluded from
acceptance:

```sh
ctest --test-dir BUILD -R '^integration-installed-embedder$' --output-on-failure
```

[Machine-readable manifest](REAL_CLIENT_M_TEST_MANIFEST.json) lists every
applicable test name, the six exceptions, exact candidates, profile counts and
SHA256 receipts for full initial runs, final batches, installed checks, TSan and
the raw header audit. Logs remain `/tmp/cog-m-gate-*-full-tests.log`,
`*-applicable.log`, `*-r18-final.log` and `*-tsan-*.log`; installed loader reports
are in each build's `installed-embedder-report.json`. The initial raw audit's
known fixture failures remain documented rather than overwritten by the later
passing maintained gate.
