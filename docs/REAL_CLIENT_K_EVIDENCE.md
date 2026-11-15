# Stage 14.K application delivery evidence

Ticket `78760e5`. Writer branch `codex/impl-application`, managed worktree
`/home/user/.codex/worktrees/impl-application/cog`. H was DONE and its reviewed
merge `c0b1c17` plus evidence `fad0337` was on main before K was claimed.
I/J/Q were integrated prerequisites. K subsequently incorporated main's reviewed
local-agent and replay changes through `5eb0c1f` without production library edits.
Independent review is child `e3bcf02`; integration/finalize is `ed4988f`.

## Delivered composition

The private application now supports generic provider poll/next/cancel/error,
typed text/tool/opaque/usage/finish assembly, one durable assistant per model
turn, strict async workspace tools, explicit policy, durable STARTED admission,
identified results and bounded continuation. It owns one finite retry budget,
timer backoff, copied queued input and limits. Main explicitly owns real HTTPS
runtime and signalfd lifecycle; no mutable global signal target remains. J's
resolved generation stays alive through provider disconnect. Real startup never
falls back to mock.

New public fixture options `mock_deltas`/`mock_delta_count` were approved as a
narrow header-first amendment (`3b5f7a5`, commit `25a6193`). The pass-2/R04 and
scenario15 traces record pointer/count bounds, borrowed lifetime, exclusivity
with legacy scripts and REAL rejection. The app invokes H's public typed mock
constructor, and uses actual core/session/toolkit/store implementations. This
fixture API neither grants effects nor bypasses durable admission.

Display state has a constructor-bounded full snapshot and transactional CGD1
codec. It includes active/failed aggregates and a retained tool result that
cannot be inserted, while excluding opaque continuation. Incomplete paths refuse
provider replay. Prequeued input remains retained after failure/cancel; only
fresh explicit postfailure submission authorizes a safe-ancestor fork. The old
failed branch remains present.

File-store parents are explicitly created/fsynced without following symlinks;
durable sessions use new incarnation ownership and a live guard. K refuses an
existing durable ID instead of overwriting history. L supplies actual resume,
operator reconciliation and fork interaction. Explicit no-store is text-only,
including unsolicited tool denial under ALLOW. Commands inherit only explicit
PATH and LC_ALL values.

## Review-cycle corrections

All four findings below were grouped within the independent source review's
first correction cycle. These are real implementation corrections, not changes
to the accepted safety contracts:

| Finding | Correction and executable regression |
|---|---|
| Earlier queued text incorrectly authorized continuation of a cancelled turn | `d79f8a8`: retain queue, require fresh inactive postfailure app_submit, preserve failed branch; revised cancel/queue test asserts send1 before authorization |
| Tool result was freed/overwritten after result insertion/save failure | `3040998`: retain terminal handle+metadata/output, freeze admission, return original error; wrapped TOOL insertion and final-save failures assert effect once, retained execution_result and no second send |
| First-user-save INDETERMINATE did not freeze subsequent input | `358086c`: centralized cog_cli_save freeze; wrapped first save asserts send0 and rejects later submit/run |
| Session insertion LIMIT hid text already displayed | `0c38ba3` + `074c463`: retain pending incomplete aggregate independently of active/provider flags, refuse further admission with original limit, avoid drain relabel/reappend; wrapped ASSISTANT append verifies exact `kept` text remains visible |

The last case reports capacity failure honestly: the aggregate is available in
memory/snapshot but could not be inserted or durably saved. No runtime growth,
partial successful snapshot, branch switch or tool effect is substituted.

## Reproduction profile

All profiles use the same audited c-ares-only curl build as F/H:

```sh
cmake -S . -B build-k -DCOG_ENABLE_WERROR=ON \
  -DCOG_NET_CURL_BUILD_DIR=/tmp/cog-net-maintained/build \
  -DCOG_NET_CURL_LIBRARY=/tmp/cog-net-maintained/build/lib/libcurl.so \
  -DCURL_LIBRARY=/tmp/cog-net-maintained/build/lib/libcurl.so \
  -DCURL_INCLUDE_DIR=/tmp/cog-net-maintained/curl-8.23.0/include
cmake --build build-k -j4
ctest --test-dir build-k \
  -R '^(cli-|integration-core-toolkit$|scenario-test_(01|02|04|05|06|12|15|16|17|20))' \
  --output-on-failure
```

Separate `build-k-asan` adds `COG_ENABLE_ASAN=ON` and runs with
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`.
Separate `build-k-clang` selects `CMAKE_C_COMPILER=clang`.
Separate `build-k-tsan` adds `COG_ENABLE_TSAN=ON`; its focused selection is
`^cli-app-(stop|limits)$`, with `TSAN_OPTIONS=halt_on_error=1`.

## Scoped results

The writer's final source checks at `074c463` are recorded below. Full target
compilation succeeds with strict GCC, Clang and ASan profiles. The exact scoped
selection contains 28 tests: 16 `cli-*`, integration-core-toolkit and eleven
original scenario executables (01/02/04/05/05b/06/12/15/16/17/20).

- Strict GCC: 28/28 PASS.
- ASan/UBSan/LSan: 28/28 PASS.
- Strict Clang: 28/28 PASS.
- TSan: 2/2 cross-thread stop/tool-limit checks PASS at the preceding source
  checkpoint; the subsequent correction only retains a failed assistant insert.

`cli-app-loop` covers successful/denied/no-store tool rounds, actual command
output/effect once, durable ledger/result and next-request association, conflict
refusal, partial-error continuation, prequeued authorization refusal, default
retry exhaustion/explicit zero retries, copied input bounds and CGD1 capacity
behavior. `cli-app-save` injects failures before admission, after effects, during
TOOL insertion, during ASSISTANT insertion and at the first uncertain user save.
It also exhausts the actual 1024-node constructor reservation with safe sibling
branches and proves the final rendered aggregate stays visible at LIMIT.
`cli-app-memory` injects each intercepted constructor allocation failure in the
mock text profile and proves a zero-Cog-allocation text run.

`cli-app-limits` exercises round exhaustion, turn deadline, real command timeout
and cross-thread cancellation/reap, strict-schema refusal, explicit truncation,
and command environment filtering. `cli-app-stop` stops during timer backoff
then verifies fresh explicitly authorized input. `cli-host-signal` drives the
actual executable with SIGINT and SIGTERM. The independent critic's
`cli-app-codec-critic` supplies golden big-endian bytes, scalar validation,
transactional destinations, every-bit mutations/truncations and 30,000
reproducible malformed frames.

`cli-app-https` runs the actual app + OpenRouter adapter + audited net against a
local TLS server. Exactly two fixture requests complete an actual atomic edit,
identified durable result, opaque-continuation resend and final text/usage.
Fragmented wire bytes and tool/result identity are asserted by the server.
The test uses a generated local CA, synthetic credential/model and disposable
workspace, with no external generation requests. **K made zero paid/live calls.**

Original scenario fixtures were migrated to explicit MOCK presence bits,
complete-turn assertions and visible cancelled snapshots. Scenario15 now drives
the actual typed app/tool loop. Scenario06 and integration-core-toolkit declare
REENTRANT|INDEPENDENT for parallel tools, zero-initialize expanded messages and
use fresh call handles before the parallel timing measurement, preserving a
meaningful overlap check under the at-most-once cached-result contract.

## Remaining gates

These scoped results do not claim the full release gate, all 38 traces, installed
transport dependency closure or L/N/O application behavior. Original07/09/10 and
the orchestrator/remote IPC families retain the broader runtime requirements
tracked for O; assertions were not skipped or replaced by fake application
handlers. Parent-owned M records bounded actual application acceptance, and P
still requires all original/R scenarios and full runtime/packaging evidence.

## Independent critic acceptance

Review `e3bcf02` accepts K's scoped delivery at
`5f9890a091227a508bad5fdae2ff0278e84b42df`, after one grouped correction cycle
(review attempt 2/4). Final production source is `074c463`; the final checkpoint
adds tests/documentation. All four production findings above were independently
reproduced before correction, then checked after correction. A real exhausted
1024-node reservation also preserves the rendered pending aggregate, with no
provider replay or further input admission. The critic supplied the independent
CGD1 golden/mutation/transactional regression now in the submission. No new
public-header amendment or production edit was made by the critic.

Independent checks used the supported curl recipe above in the separate managed
`review-application-k` worktree. Full target compilation succeeded under strict
GCC, strict Clang and Clang ASan/UBSan. At the final checkpoint the exact 28-test
K selection above passed 28/28 in each profile; sanitizer runs enabled leak
checking and halt-on-error. The public CLI header also compiled standalone under
both strict compilers. Four concurrency selections passed under a separate GCC
TSan build at `074c463`:

```sh
TSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-k-critic-tsan \
  -R '^(cli-app-stop|cli-app-limits|integration-core-toolkit|scenario-test_06_parallel_tools)$' \
  --output-on-failure
```

Broader checks selected 90 registered tests using this explicit exclusion:

```sh
ctest --test-dir build-k-critic \
  -E '^(unit-orchestrator|integration-orchestrator-(ipc|stress)|scenario-test_(07_aux_crash|09_subagent_messaging|10_fan_out_fan_in))$' \
  --output-on-failure -j4
```

That selection passed 90/90 under GCC at `074c463`. Earlier complete 90-test
Clang and ASan runs at `f085d69` also passed. During concurrent final GCC/Clang/
ASan validation, Clang and ASan each passed 89/90: the former aborted at
`test_toolkit_review_faults.c:168` expecting no KILL attempt within a 150 ms TERM
grace, and the latter at `test_workspace.c:212` expecting a grandchild PID after
a 40 ms startup timeout or a 15 ms pre-cancel sleep. Both unchanged fixture tests
subsequently passed five serial repetitions; those reruns do not erase the
observed failures or establish deterministic release validation. P follow-up
`b4c9ca3` requires bounded fixture handshakes and retained cleanup assertions
under concurrent load. No memory, undefined-behavior or race diagnostic occurred
in these checks.

The six excluded executables were run and failed, not assumed irrelevant. O
follow-up `d5280b7` records their concrete causes: self-connected/auxiliary
fixtures expect synchronous connect OK although approved IPC connect returns
AGAIN, and production orchestrator accept/health/receive/shutdown paths contain
no `cog_ipc_endpoint_service` calls. The built helper exists; this requires
bounded runtime and fixture migration, not a missing executable/path fix. Full
M/P all-unit and runtime acceptance remains dependent on that work. K's scoped
PASS does not close O, the timing follow-up, L, M or the full release gate.
All review requests used deterministic local fixtures and disposable processes;
no paid/provider calls or credentials were used.

## Main integration

Authorized integration merged the reviewed K checkpoint plus independent evidence
(`b7bf456`) into main as `b20ac9e4097707d3b0b4f278aad783c4eb7d4bd3`, without
conflicts. The integration preserves the preceding Release assertion-policy
merge `db60296`. A fresh supported-curl Release/Werror configuration in
`build-k-main-merge` compiled every target and passed the exact 28-test K
selection above (28/28), plus all six `build-policy` checks (6/6). The application
capacity test's actual compile flags include `-O3 -DNDEBUG ... -UNDEBUG`, so
assertion-contained workloads remain enabled in this combined Release tree.

Only K delivery, its independent review and integration/finalize children close
with this result. O nonblocking orchestration migration (`d5280b7`), P process
fixture determinism (`b4c9ca3`), L/M and full release acceptance remain separate.
The existing untracked `build_user/` directory was preserved.
