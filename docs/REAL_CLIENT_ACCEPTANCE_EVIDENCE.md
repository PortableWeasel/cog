# Focused application acceptance joins

Ticket `83b30aa`, audit `267dd8b`; implementation base `84e2272`.
The combined source checkpoint is `c6692562f7add196f403cfb2cf9d17a39d287bf7`.
It includes the independently approved host correction `27da63e`, its contract
and evidence follow-up `36fdd14`, and independent adapter fixtures `b22a21d` /
`a5c1c5f`. Independent acceptance review passed on this source checkpoint;
integration and the broader M/N/O/P gates remain separate.

These cases join application ownership, real dispatch, durable state and wire
behavior where the earlier component tests did not prove the complete assertion.
They do not replace the twenty scenario tests or claim live-provider acceptance.
No paid calls or real credentials are used. REAL cases use a local TLS server,
synthetic credentials and the actual OpenRouter adapter, net and admitted runtime.
MOCK is explicitly selected only in deterministic application/terminal fixtures.

## Exact assertion map

| Required join / related scenarios | Fixture and assertion |
|---|---|
| ASK / 06, 15 | `cli-client-acceptance`: noninteractive ASK denies both identified calls without an effect; interactive arbitrary input and queued user text leave the decision undecided; explicit allow runs the first complete call once, explicit deny refuses the second. At the actual next provider send, load the durable checkpoint and verify both results and ledger/audit states already exist. |
| Undeliverable ASK / 15, 16 | `cli-client-ask-output-initial` and `-repeat`: redirect stderr to `/dev/full` at owner-service policy readiness, before initial delivery or the repeat prompt. Initial case already has `allow` in the real stdin pipe. IO stops admission; the marker is absent and no audit enters STARTED. The repeat case passes arbitrary input through the actual terminal parser. |
| Retry elapsed and minimum deadline / 02, 12 | `cli-client-acceptance`: real mock error metadata requests a 10 ms retry. At elapsed 0 and elapsed 5, equality with either remaining retry or whole-turn budget denies another send; one extra millisecond admits exactly one retry. The real BACKOFF service deadline equals retry_at, the earlier deadline. Final status and exact send count are checked. Only the explicit service clock input changes; dispatch and provider results remain real. |
| Shared whole-turn budget / 02, 06, 12, 15 | `cli-client-wire-budget`: real HTTP429, retry, real command tool and follow-up send retain the original started_at. Seven live queued inputs remain intact while repeated valid USER_INPUT events keep ordinary core queue work present. The service deadline still equals original start + max_turn_ms after retry and tool time. One millisecond before that boundary admits the continuation; the exact boundary produces TIMEOUT and drains before another HTTP request reaches the server. Three provider sends, two HTTP requests, one successful tool result and one effect marker are asserted. |
| Cumulative context / 01, 15 | `cli-client-acceptance`: complete a real durable command round with text, call identity/name/arguments, result and binary opaque ancestor. Independently sum concrete fixture text, schema, IDs/names/arguments/result and exact opaque namespace/format/blob. Resume with precisely that capacity admits one send with five intact messages. One byte less admits zero sends; old messages, opaque bytes and the successful tool ledger remain intact. No pruning helper supplies the oracle. |
| Semantic finish / 01, 15, 16 | `cli-client-acceptance`: accepted text plus a complete tool proposal ending in LENGTH or CONTENT_FILTER yields LIMIT and one INCOMPLETE assistant. There is one send, no tool ledger and no effect. |
| DNS cancellation / 02, 19, 20 | `cli-client-wire-cancel-dns`: a local UDP blackhole receives the actual named query before marking readiness. A test-only easy constructor sets CURLOPT_DNS_SERVERS on the genuine net-owned easy handle. Actual curl/c-ares perform all resolution and cancellation; no resolver/completion replacement, preload or resolver thread is used. |
| Connect/TLS cancellation / 02, 19, 20 | `cli-client-wire-cancel-connect` fills a local listen backlog and observes the actual destination SYN_SENT entry before cancellation. `-cancel-tls` accepts the socket and receives actual TLS ClientHello bytes before readiness. Neither case sends an HTTP request. |
| Accepted/full retained output / 01, 19, 20 | `cli-client-wire-cancel-stream` waits for the actual app to accept partial text. `-cancel-retained` reserves exactly one ordinary adapter entry through a test-only constructor policy; nonconsuming next(NULL,0) OVERFLOW proves the entry is occupied before cancel is enqueued. Retained text appears exactly once and the terminal cancellation still drains. |
| Retained success wins / 01, 20 | `cli-client-wire-cancel-finish` waits for the actual successful FINISH retained in the app, after the net slot has been released, then enqueues cancel. The assistant is COMPLETE once, with all eight text bytes and one send. |
| Completed-tool opaque ancestor fork / 13, 14, 15 | `cli-client-wire-fork`: first app executes one command and saves opaque continuation + identified result. A fresh FORK app selects the completed tool-result ancestor. The server checks exact ancestor reasoning_details, call/result pairing and exclusion of the old final leaf; the marker is unchanged before the new command. Reusing the textual call ID creates a new assistant ledger scope. Four HTTP requests, two distinct successful ledger identities, marker `oncebranch`, and the preserved old final leaf prove no old-effect replay. |
| Cancel/drain lifetime / 05, 19, 20 | Every wire mode ends with provider inactive, no execution handle and zero net slots. Cancellation cases send once and never retry. Thread count remains one; complete teardown restores FD/thread baselines. A new RESUME constructor acquires the same durable store guard after teardown without sending. |
| Installed full output and signals / 05, 20 | `cli-client-installed-output`: normal runtime-component install, actual core.run_start readiness and SigBlk verification, then a full4096-byte stdout/stderr pipe. Separate write-first and signal-first cases cover both SIGINT and SIGTERM. Write-first accepts checked early IO; if a blocking write is observed it must still exit within the signal bound. Signal-first sends the actual masked signal while the pipe is already saturated. Parent OFD flags are identical before/after. Seeded checkpoint RESUME after each exit proves ownership release. Regular stdout/stderr files preserve the inherited shared offset and exact prefix. |
| Output adapter faults | `cli-host-output-critic`: five isolated independent cases cover zero/repeated close, second FILE-constructor NOMEM rollback and FD restoration, regular offsets/bindings, real accepted short prefix without replay, EINTR without retry, and full blocking socket without shared flag mutation. Children use exit so LSan runs. |

Existing `cli-host-signal` and `cli-installed-session-ux` are rerun alongside the
new fixtures: idle signalfd, EPIPE/closed-reader cleanup and ordinary installed
restart/fork/recovery remain covered without duplicating their implementations.

## Defects reproduced and routed

Before `02678a1`, `probe_full_pipe_signal.py` established an actual write(1/2),
full pipe and masked INT/TERM using `/proc/PID/syscall`, FIONREAD and SigBlk.
All four stream/signal combinations remained blocked with the corresponding
ShdPnd bit; draining the pipe let each exit zero. No executable caller precondition
required nonblocking output. This original-baseline diagnostic intentionally
requires the blocked-write state; the registered installed regression permits
the corrected checked-IO early exit instead.

Both actual ASK failure probes originally returned IO only **after** an effect
and STARTED audit. The separately reviewed production correction checks fprintf
and fflush before publishing initial policy readiness or continuing input.
Corrected assertions require IO, absent marker, and no STARTED transition.

An idle fresh durable session with no first input also revealed a separate empty
owner reservation issue (later RESUME not-found / NEW same ID invalid-state),
routed as `cd3d092`. Host output tests seed a checkpoint first to isolate their
ownership assertion. This document does not claim that independent issue fixed.

## Build and runtime evidence

All-target strict Release builds pass with GCC 14.2.0 and Clang 19.1.7, and with
Clang Release ASan+UBSan. Assertions remain active through the project test-only
`-UNDEBUG` policy; production flags are unchanged. No allocator wrapper policy is
added by these tests. The independent adapter fault fixture wraps only write and
fopencookie. ASan uses `detect_leaks=1:halt_on_error=1`; UBSan uses
`halt_on_error=1:print_stacktrace=1`.

Before host composition, the nine focused cases passed under all three builds.
After composition, all fifteen selected cases pass under all three builds.
The final initial-ASK prequeued-input adjustment was additionally rebuilt and
rerun under each compiler/sanitizer. One sanitizer run exposed a fixture-only
PermissionError when LSan temporarily owns ptrace during exit: inaccessible proc
data is never readiness; the same bounded poll now waits for real waitpid exit.
The corrected full selected suite passed afterward.

Build directories are `/tmp/cog-acceptance-gcc`, `/tmp/cog-acceptance-clang` and
`/tmp/cog-acceptance-asan`. The selected test expression is:

```sh
ctest --test-dir BUILD --output-on-failure -j4 \
  -R '^cli-client-|^cli-host-output-critic$|^cli-host-signal$|^cli-installed-session-ux$'
```

Configuration pins the frozen accepted profile:

```sh
cmake -S . -B BUILD -DCMAKE_BUILD_TYPE=Release -DCOG_ENABLE_WERROR=ON \
  -DCMAKE_PREFIX_PATH=/tmp/cog-relocatable-profile/shipped/curl-build/install \
  -DCURL_DIR=/tmp/cog-relocatable-profile/shipped/curl-build/install/lib/cmake/CURL \
  -DCURL_LIBRARY=/tmp/cog-relocatable-profile/shipped/curl-build/install/lib/libcurl.so \
  -DCURL_INCLUDE_DIR=/tmp/cog-relocatable-profile/shipped/curl-build/install/include \
  -DCOG_NET_CURL_BUILD_DIR=/tmp/cog-relocatable-profile/shipped/curl-build \
  -DCOG_NET_CURL_LIBRARY=/tmp/cog-relocatable-profile/shipped/curl-build/install/lib/libcurl.so
cmake --build BUILD -j4
```

Clang adds `-DCMAKE_C_COMPILER=clang`; sanitizer adds `-DCOG_ENABLE_ASAN=ON` in a
separate directory. CURL Config mode supplies the imported target and reports
the explicit CURL_LIBRARY/INCLUDE_DIR cache hints unused; its actual include and
loaded runtime resolve to this same prefix. Runtime ldd confirms libcurl and
c-ares both load there, without LD_LIBRARY_PATH. Final frozen hashes remain:

| Artifact | SHA256 |
|---|---|
| curl 8.23.0 libcurl.so.4.8.0 | `ae1a9e77488254d744c9064972b025d8eb7210e73ea900cd5f3632995bb29d18` |
| c-ares 1.34.9 libcares.so.2 | `b4d2377241c646dc2054e864e403f162d4b5969823bdd86ec5ff49ecf7e79f53` |

The installed MOCK host fixture installs only cog-cli-runtime for offline host
semantics. Full installed real dependency closure remains the separately
accepted R18 fixture/provenance evidence; this host fixture makes no replacement
claim. Linux `/proc` readiness observations are bounded test handshakes rather
than portable application APIs or hard real-time performance claims.

## Independent final review

A different critic reviewed the complete `c669256` source and `e1f6055` evidence
in a managed isolated checkout. No production revision was required after
`27da63e`; the installed stderr test's initial readiness failure was corrected
by selecting a guaranteed checked reload notice rather than assuming mock usage
would print. The final runner explicitly waits for real process completion when
LSan temporarily prevents `/proc/PID/syscall` access.

Fresh all-target strict Release builds passed under GCC and Clang, plus a
separate Clang ASan/UBSan build with leak detection enabled. All **41 tests**
selected by `ctest -R 'cli-|header' --output-on-failure -j4` passed in each build.
The independent build directories are `/tmp/cog-host-review-gcc`,
`/tmp/cog-host-review-clang` and `/tmp/cog-host-review-asan`. The final trees are
identical to the submitted source; test-only `-UNDEBUG` was verified in the
Release helper fixture's actual compile flags. The reviewer also ran the five
private adapter fault cases separately under strict GCC, strict Clang and
Clang ASan/UBSan/LSan before integration.

This is approval of the focused acceptance joins and host correction, not a
full-suite or original-scenario gate. It does not close the separately routed
empty-session issue `cd3d092`, the M live smoke, or N/O remote orchestration and
P release validation. No paid requests or real credentials were used.

Integration retained the approved source history and cherry-picked only the
independent evidence. The resulting `7f686f0` tree exactly matched the reviewed
tree. A fresh GCC Release all-target build from main in
`/tmp/cog-acceptance-main` passed the same **41/41** CLI/header tests, including
the genuine installed output and existing EPIPE/recovery checks.
