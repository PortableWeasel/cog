# Stage 14.L session and terminal delivery evidence

Ticket `ff2dbc9`. Writer branch `codex/impl-session-ux`, managed worktree
`/home/user/.codex/worktrees/impl-session-ux/cog`. K was integrated before L
started. L incorporated approved configuration tip `24305da`, provider-preflight
tip `b0e34e3` through merge `9f62f1f`, and current main `5941289` through
`72ec007`. Independent implementation review is `e509146`; integration/finalize
is `631d004`. This writer evidence does not substitute for either verdict.

## Header gates and implementation

L's three header passes are `3569843`, `0e98751`, and `99bbc5b`, followed by
independently reviewed clarifications at `fa56242` (`65ccb86`, PASS). They define
explicit selection, offline metadata admission, exact ownership activation,
operator-fenced migration and per-call recovery. Fourteen targeted traces in
[L contracts](REAL_CLIENT_L_CONTRACTS.md) cover their composition with G/K.

The restored-PROPOSED compatibility gap could not be implemented through
structural validation or a send probe. A separate three-pass, independently
approved generic provider-preflight amendment (`495567a`, `f7d9933`) and runtime
delivery (`396a690`, final `b0e34e3`) supply the required no-wire ninth callback.
Its independent evidence is [recorded separately](REAL_CLIENT_PREFLIGHT_EVIDENCE.md).
The app uses that public callback with PENDING_TOOLS and the complete selected
prefix/current settings before policy admission. It never parses adapter opaque
data or synthesizes missing TOOL results to make a send probe possible.

Private `app_sessions.c` handles LIST, exact restore/admission, same-session fork,
offline reconciliation and terminal commands. `cli.c` retains lifecycle ownership;
`app_turn.c` supplies bounded startup policy dispatch and fresh-input rules. Main
explicitly handles metadata without net initialization, signalfd and SIGPIPE
disposition lifetimes, checked output closure and teardown. No production core,
store or toolkit implementation was changed by L. Provider changes were isolated,
header-gated and independently reviewed before integration.

LIST is model/credential free, reports metadata names rather than health, and
validates the complete enumeration before output. RESUME restores the exact
checkpoint into current reservations, advances a checked owner epoch with a fresh
incarnation and acquires a real exclusive guard. Competing guards refuse access.
Incomplete text retains K's explicit fresh postfailure continuation semantics.
FORK preserves all branches and effect identities; fresh input creates the new
child at the selected safe parent.

RECOVER requires the operator's explicit fencing assertion and a recovery-only
guard. The assertion does not verify process death. Reconciliation identifies
one uncertain call, records bounded evidence plus result/audit and saves before
reporting success. Save failure freezes later reconciliation and retains the
guard. Ordinary prompts, ALLOW policy and queued text cannot execute tools in
this mode. After every uncertainty closes, only a later explicit RESUME enables
normal work in a new epoch. Unowned uncertain generations remain refused.

## Review-cycle corrections

The early independent source review at `f21a059` grouped three concrete terminal
findings. `0eec4a9` fixes them and adds permanent regressions:

| Finding | Correction and observable check |
|---|---|
| Unknown usage returned before detecting buffered output failure | Checked flush; `/dev/full` returns IO for unavailable usage |
| Recovery ignored poll descriptor error bits | ERR/NVAL and exceptional signal reads return IO; closing stdin after construction terminates with IO |
| Command parsing could ignore bytes after embedded NUL | Validate the entire UTF-8/no-NUL line first; malformed reconcile/fork input preserves checkpoint, ledger, branch and send count |

A separate bounded reachability trace, [L01](scenarios/L01-terminal-reconcile-identity.md),
identified valid tool IDs that cannot be represented as a literal whitespace
token. `688c762` adds `/reconcile-index NODE ORDINAL OUTCOME EVIDENCE`; ordinals
are 1-based positions in the immutable original assistant call array. `/tools`
prints escaped IDs and those stable ordinals. Literal `/reconcile` never treats
an `@`-looking ID as an index. Tests cover space/newline IDs, literal `@2`, invalid
and overflowing ordinals without mutation, and unchanged positions after earlier
calls close. No public API or additional recovery authority was introduced.

## Reproduction profile

All profiles use the audited c-ares-only curl build used by F/H/K:

```sh
cmake -S . -B build-l -DCMAKE_BUILD_TYPE=Release -DCOG_ENABLE_WERROR=ON \
  -DCOG_NET_CURL_BUILD_DIR=/tmp/cog-net-maintained/build \
  -DCOG_NET_CURL_LIBRARY=/tmp/cog-net-maintained/build/lib/libcurl.so \
  -DCURL_LIBRARY=/tmp/cog-net-maintained/build/lib/libcurl.so \
  -DCURL_INCLUDE_DIR=/tmp/cog-net-maintained/curl-8.23.0/include
cmake --build build-l -j4
ctest --test-dir build-l \
  -R '^(cli-|integration-core-toolkit$|scenario-test_(01|02|04|05|06|12|15|16|17|20))' \
  --output-on-failure -j4
```

`build-l-clang` selects `CMAKE_C_COMPILER=clang` with Release. Separate
`build-l-asan` selects Clang, Debug and `COG_ENABLE_ASAN=ON`, and runs with
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. Separate `build-l-tsan`
selects GCC/Debug and `COG_ENABLE_TSAN=ON`; its focused selection is
`^cli-app-(stop|limits)$` with `TSAN_OPTIONS=halt_on_error=1`.

## Scoped writer checks

At final executable checkpoint `b3b8477`, full target compilation and the exact
31-test selection above pass under strict GCC Release, strict Clang Release and
Clang ASan/UBSan/LSan. The selection consists of 19 CLI tests, one core/toolkit
integration and eleven original scenario executables. TSan's two cross-thread
stop/tool-limit checks pass at `6c67500`; subsequent changes merge already-reviewed
toolkit fixtures, add a test-only effect sentinel and update documentation.

The new tests use actual public core/store/provider/toolkit implementations:

- `cli-session-ux`: durable restart with epoch advance; competing guard refusal;
  divergent branches with preserved old nodes; retained partial text and fresh
  authorization; offline metadata without model/credential callbacks; two-call
  persisted STARTED recovery and separate audit/result saves; explicit empty
  text-only v1 migration; list/output capacity failures; known/unknown usage;
  terminal byte-validation regressions and stable ordinal identity selection.
- `cli-session-recovery-faults`: injected IO and INDETERMINATE recovery saves
  retain ownership and freeze later reconciliation, leave the committed
  checkpoint unchanged, refuse a competitor, then restore uncertainty on a fresh
  constructor. A later explicit acknowledgement persists without execution.
- `cli-installed-session-ux`: installs the actual executable with CMake's
  `cog-cli-runtime` component into a disposable prefix, then drives separate
  processes through create/list/resume/fork, visible history/usage, final-line
  EOF, missing paths/IDs, unsafe-resume refusal, offline per-call reconciliation,
  broken stdout and recovery SIGTERM followed by successful guard reacquisition.

The restored-PROPOSED mock case actually runs one approved disposable command,
observes exactly `once`, skips a previously completed command that would append
`BAD`, preserves opaque bytes and sends only the completed continuation. DENY and
pre-policy cancel do not execute the remaining proposal. The REAL adapter case
uses an audited local net context, synthetic credential/model and loopback URL,
with no wire request. Foreign opaque namespace returns UNSUPPORTED; malformed
known `reasoning.encrypted.data:42` returns PROTOCOL. For both refusals, an exact
workspace file initialized to `sentinel` remains byte-for-byte unchanged and net
active count remains zero. Both restored commands would append to that file if
effects were admitted. A supported prefix cancelled before policy also leaves
the marker unchanged. **L made zero paid/live generation calls.** Existing
`cli-app-https` still passes actual local TLS app/tool continuation composition.

## Boundaries and remaining gates

Persisted STARTED fixtures model uncertain prior effects; the application does
not claim to kill or verify old processes. The installed runtime component tests
offline restart behavior and does not prove shipped real-transport dependency
closure. P retains packaging, all 38 traces and full-suite requirements. N/O retain
remote/headless/agent application integration; no fake handlers or scenario skips
were added to claim those gates.

This delivery retains one immutable J/K configuration/provider per app, with
request/preflight generation 1. Live configuration reload, publication/retirement
and core provider-view lifetime are separate design/delivery follow-ups
`dffc59e`/`af6956a`. M must include that applicable work before acceptance. L's
independent final review and parent integration remain separate from these
writer checks.
