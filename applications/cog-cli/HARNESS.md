# Application scenario harness

Tests link `cog::cli` and drive the real application, core, session, provider
callbacks, toolkit and durable store. MOCK supplies deterministic provider
output; it does not replace tool execution or permit effects. The actual
adapter/net composition has an offline TLS test (`cli-app-https`).

Start every options/message/config value at zero. Programmatic options must
set presence bits; an endpoint string alone does not select the mock backend:

```c
cog_cli_options_t opts = {
    .backend = COG_CLI_BACKEND_MOCK,
    .explicit_options = COG_CLI_OPT_BACKEND,
    .prompt = "hello",
};
cog_cli_app_t *app = NULL;
CHECK(cog_cli_app_new(&app, &opts) == COG_OK);
CHECK(cog_cli_app_run(app) == COG_OK);
cog_cli_app_free(app);
```

Legacy `script` supplies CHUNK/ERROR/CANCEL/COMPLETE steps, consumed across sends.
An error followed by a chunk proves a retry only when the error precedes accepted
output. Two retryable errors exhaust the default single retry. Generic error
snapshots are valid until the next send; do not assert stale metadata after a
successful retry. Exact send/error counters remain observable.

For structured turns, use `opts.mock_deltas` and `mock_delta_count` (at most 4096
borrowed entries). They are MOCK-only and mutually exclusive with legacy script.
Keep all nested strings/deltas alive until app destruction. The app uses the
public `cog_mock_provider_new_deltas` constructor. Each fixture turn ends in a
FINISH. A complete tool turn is BEGIN, ARGUMENTS, END, FINISH/TOOL_CALLS; subsequent
TEXT + FINISH/STOP supplies the model continuation. Use strict built-in schemas.
`test_app_loop.c` and scenario15 are working full-round examples.

For effect tests, create a disposable `mkdtemp` workspace and durable store;
set WORKSPACE, STORE_MODE, STORE_DIR and POLICY presence bits. Explicit ALLOW is
preapproved only within those fixture roots. Assert the actual file/process
result and durable identified ledger, including the matching result in the next
provider request. DENY and no-store tests assert absence of the effect. Never
use a legacy synchronous tool as a shortcut around durable async admission.

| Injection | Contract |
|---|---|
| `app_submit` | Copies UTF-8 text into a bounded slot through handler completion |
| `app_queue` | Retains copied input for a later safe turn; cannot authorize failed-turn continuation |
| `app_cancel` | Enqueues owner-thread cancellation and retains explicit cancelled display state |
| `app_shutdown` | Stops admission, cancels/drains/reaps, saves outcomes, then stops |
| `app_stop` | Only cross-thread/signal-safe operation; run drains after core exits |

One completed assistant message contains all text chunks. Inspect complete
provider history with `cog_session_copy_history`; incomplete or unresolved
history must refuse replay. Use `app_display_snapshot` to inspect cancelled,
partial or unresolved visible state. The application snapshot never exposes
opaque continuation. For retained failed branches use `cog_session_snapshot_view`
and verify the old node still exists after a freshly authorized continuation.
Prequeued input must not cause that continuation automatically.

Failure-injection tests wrap allocation, user/STARTED/result saves and TOOL
insertion. They verify zero effects before durable STARTED, a retained terminal
result after recording failure, no second provider send and immediate freeze
following uncertain persistence. Tool tests also cover schema refusal, finite
round/turn deadlines, explicit truncation, sanitized command environment,
cancellation and child reap. Every temporary directory has a unique prefix;
cleanup only operates on its owned root/PIDs.

The scoped K selection is:

```sh
ctest --test-dir build-k \
  -R '^(cli-|scenario-test_(01|02|04|05|06|12|15|16|17|20))' \
  --output-on-failure
```

This selects 27 checks with the audited transport profile. Original03/07/09/10
are broader agent/IPC/orchestrator scenarios; app fixture migrations preserve
those assertions while O supplies their remaining runtime composition. The full
release gate still runs every original scenario plus R01–R18; this selection is
not that gate. Sanitizers use separate build directories. The focused TSan run
covers `cli-app-stop` and `cli-app-limits` with real cross-thread stop calls.


L's `cli-session-ux` uses disposable durable stores through the actual app and
public G operations. It tests exact restart, fresh epochs, lock contention,
same-tree divergent branches, partial-turn authorization, per-call recovery,
legacy migration, known/unknown usage and output/input errors. Its seed mode
constructs coherent synthetic prior intent; it never executes those historical
fixture commands. The pending-phase cases retain a completed first result and
prove only the remaining PROPOSED call can start after provider preflight.
Audited-profile cases use actual OpenRouter preflight with synthetic credentials
and a local refused-port endpoint; unsupported/malformed opaque history must fail
with no wire request or tool effect. A valid known opaque form is cancelled before
policy and also starts no request.

`cli-installed-session-ux` uses `cmake --install --component cog-cli-runtime` into
an owned temporary prefix and drives the installed executable. It covers offline
credential-free listing, restart/fork/history, EOF, explicit per-call recovery,
known usage, broken stdout and metadata recovery signals. All conversations use
explicit MOCK; no paid/live request or model substitution is used. The fixture
helper creates uncertain v2 records with public store APIs and then exits, so the
test harness can honestly assert its old fixture processes are quiescent.

`cli-session-recovery-faults` wraps failed/uncertain recovery saves. It verifies
that the guard remains held, a competing app refuses admission, the checkpoint
has not been replaced by the injected failure, no call is reexecuted, and only a
new explicit recovery decision after restart can close the durable uncertainty.
Use separate sanitizer directories and preserve these failure paths in Release;
assertion workloads are kept enabled by the shared test build policy.

`cli-app-reload` uses real app/config/provider objects with explicit owner-hook
controls between dispatches. It exercises two providers, retained OVERFLOW,
whole-turn retry/tool/policy pinning, pending restore, real validator rejection
and actual reset-LIMIT, consuming cleanup faults, source/resolver lifetime,
known-prefix REAL compatibility and queue/EOF/fresh-input boundaries. Linker
interposition supplies failure injection while still running the real commit,
disconnect and copied-generation implementations. No paid request is made.
`cli-app-reload-critic` independently checks pipe EOF during delayed old work and
64 successful publications, with an exact two-live-owner peak and65 total
constructors/destructors. Original08 preserves its component assertions and now
also drives actual app generation1/2 sends. Installed session tests additionally
drive terminal reload; app-memory tests exhaust explicit candidate constructors
and reject all allocations during later activation/service/send.

The exact R12 selection and supported curl recipe are in
[R12 evidence](../../docs/REAL_CLIENT_RELOAD_EVIDENCE.md). Use separate Release
GCC/Clang/ASan and Debug TSan directories. The selected executable count is not
a claim that all release scenarios or full installed transport closure passed.

`cli-observation-{normal,retry,stall,tls,connect,cancel}` uses the actual supported
curl/OpenRouter/app/tool path against a disposable local TLS server. Test sinks
copy bounded events, verify that delivery occurs outside the net callback and
reject payload/credential markers. The normal path executes one real edit, checks
its durable identified result and makes a generation2 request after reload with a
copied127-byte model label. The retry path first receives HTTP429: only the admitted
new attempt has RETRY, at its OPEN timestamp. Known usage snapshots replace rather
than add, and the last generation's absent usage remains unavailable.

Failure modes exercise actual response stall, trust failure, refused connection
and cancellation after accepted partial text. They require one transport terminal,
sanitized classification, checked failed duration and no invented tool execution.
Original18/19 retain their component assertions and also run the actual retry/tool/
reload fixture under the admitted Linux profile. These production paths supplement
the component tests' synthetic event construction.

`cli-observation-critic` independently exercises retained IDs, row exhaustion across
contexts, final app consumption with foreign live/retained IDs and later callbacks,
raw observer AGAIN, deferred sink failure, checked-clock failure and queue overflow.
`cli-app-memory` exhausts each explicit bridge constructor allocation and verifies
NULL rollback plus overflow/zero bounds, using the established optimized allocator
positive control. Its existing allocation-free mock run and reload activation
checks continue to execute the new app timing paths. Exact selected commands and
supported dependency identities are in
[observation evidence](../../docs/REAL_CLIENT_OBSERVATION_EVIDENCE.md).

`cli-empty-session` verifies successful empty checkpoint publication before app
construction returns, exact guarded repair of an interrupted owner-only reservation
and zero provider sends. A real child holds the old guard, proves live contention,
then exits without releasing it; subsequent explicit RESUME uses a new epoch and
commits an empty snapshot. Before/after-publication IO/INDETERMINATE are tested for
both NEW and owner-only RESUME, alongside CAS uncertainty/races, acquire failure,
epoch exhaustion, selector/collision refusal, unchanged legacy remnants and stored
model/provider provenance under current J configuration. Fault wrappers exercise
existing store operations; they do not replace admission or effect logic.

`cli-installed-empty-session` normally installs the executable runtime component
and exercises actual idle EOF, SIGINT and SIGTERM before any input. It waits for
real core readiness and the host's blocked-signal mask, holds stdin open until
signal exit and then resumes each session. A public-store fixture also seeds an
old owner-only reservation; the installed host repairs it. Public snapshot/owner
inspection checks zero nodes/ledger/identity high-water, exact epoch advance and
unchanged committed empty checkpoint on ordinary restart. This runtime-component
test is separate from R18's full dependency-install/relocation closure. See
[empty session evidence](../../docs/REAL_CLIENT_EMPTY_SESSION.md).
