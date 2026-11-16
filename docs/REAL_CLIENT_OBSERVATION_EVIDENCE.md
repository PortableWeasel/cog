# Application observation delivery

Parent `b64ee7c`; private interface `ba4ec54`, bridge `2b727a2`, lifecycle
`aed1db8`, fixtures `31a0e61`, independent review `571c7b5`, integration `3d2ca4a`.
Source checkpoint `b6b2141`, fixture checkpoint `02bd877`; independent final
review and main integration remain pending at this writer checkpoint.

The [approved contract](REAL_CLIENT_OBSERVABILITY_CONTRACTS.md) is implemented by
the private [bridge](../applications/cog-cli/src/app_observation.c), host and app
lifecycle hooks. No public header, net/provider/core implementation or dependency
admission rule changed. The accepted packaging/profile history through main
`0a36eb8` is retained in merge `7c0752d`.

## Interface and implementation

Private header passes `b69ab47`, `23fa50a`, `9213799` precede implementation.
Fourteen strict C11/C++11 checks passed with GCC/Clang/G++/Clang++. The
[private trace](REAL_CLIENT_OBSERVATION_PRIVATE.md) maps fifteen approved flows
to explicit calls, scope and lifetime. Independent cycle1 found retained-ID and
failed-detach bugs plus raw observer AGAIN being treated as core no-progress.
Header amendment `45732a6` precedes grouped source correction `9590bee`.

The host reserves four rows/eighteen queue entries before net construction. Its
callback only checks the monotonic clock and copies numeric values/prepared safe
scope. Owner drain publishes existing diagnostics/events and DEBUG records after
every provider operation, including failure and consuming disconnect. It assigns
nonreused connection identities across context/request pairs. Rows never evict an
active or terminal-retained request; request_status must report NOT_FOUND before
reuse. Owners release all IDs, including any unobserved after a feed loss, before
clearing a reusable domain.

Final app destruction has a separate irreversible target-detach path. It clears
diagnostic borrows while obs is alive, latches an incomplete-feed error and ends
the target borrow without cancelling foreign work. Late callbacks encounter
sticky failure and never touch freed app/obs storage. Host bridge storage survives
all borrowing net contexts. Callback status remains distinct from deferred sink/
format/timing failure. Both stop admission; observation AGAIN is fatal app IO.
Primary work error wins, ordinary cleanup persists/reaps effects, and no prior
wire or tool result becomes replayable.

Provider and tool spans use static names and checked durations. Provider usage
is a cumulative replacement snapshot; numeric values are omitted when unavailable.
Tool timing covers admitted execution through joined reap; refusal has an explicit
no-execution record. Identified tool-terminal once state survives retained result
insertion failure. Typed field descriptors are immutable static data. Separate
bounded usage records fit existing scratch even with two maximum-length labels.

Private configuration constructors cache safe optional labels after credential
resolution. Allowed ASCII labels fit127 bytes plus NUL; long/unsafe/unavailable or
credential-containing labels are absent, without modifying effective options.
No secret getter or retained resolver authority is added. Real empty/long secrets
make optional labels unavailable. Auto-generated session labels are unavailable;
explicit safe session/source IDs may be copied. Default streamed UI is unchanged;
verbose or an explicit log path admits DEBUG records. File delivery flushes and
checks errors outside callbacks.

## Production-path and failure oracles

| Fixture | Concrete assertion |
|---|---|
| normal local TLS | Actual OpenRouter sends, real edit_file effect and one SUCCEEDED durable ledger entry; provider/tool spans have positive durations and close fully |
| HTTP429 retry | Four real requests across tool continuation/reload; first CLOSED is transport OK/HTTP429, only admitted second OPEN gets RETRY with the identical captured timestamp |
| event order/accounting | Unique connection IDs, OPEN before actual CONNECTED, nondecreasing callback timestamps, exact progress delta/total consistency and one terminal per request |
| copied labels/reload | Old secret-like model label omitted; old rows survive return/retirement; generation2 gets a copied127-byte model despite immediate source mutation; session127 bytes fits |
| usage | Reported2/3 input/output replaces prior snapshots; later absent usage has unavailable flags and no numeric values; unknown cost is not zero |
| privacy | Sink rejects credential/header/body/tool-path/output/opaque markers; sanitized static endpoint/provider and transport classes only; no sink executes inside capture callback |
| response stall | Actual HTTP response stalls, STALLED then FAILED/TIMEOUT, one failed timed provider attempt |
| trust/connect failure | Actual certificate trust failure/refused local port has no CONNECTED and emits the appropriate sanitized transport class |
| active cancel | Owner cancels after accepted partial model text while wire remains open; one ABORTED terminal, one failed provider span and no tool execution |
| original18/19 | Original component assertions retained, plus actual retry/tool/reload composition under admitted profile |
| critic ownership | Actual retained request prevents clear/unbind/reuse; release enables reuse; independent context cannot evict retained row |
| critic teardown | Consuming app_free with live or retained foreign work; later actual callback cannot access freed target; foreign IDs remain owned until host releases |
| critic lost feed | Raw external AGAIN is fatal app/core IO; primary PROTOCOL preserved; deferred sink failure stays separate from raw net status |
| critic bounds | Callback clock failure and actual queue exhaustion explicitly invalidate feed without retry or partial replay |
| constructor OOM | Three constructor allocation failures unwind to NULL; zero/overflow/NULL bounds; established optimized allocator positive control remains active |
| existing app regressions | Save/uncertain persistence, retained output/results, incomplete fresh-input semantics, stop, recovery/fork and R12 two-owner generation tests continue passing |

The normal fixture injects only obs consumers through constructor interposition
and owner-thread cancellation through the existing service hook. Provider, TLS,
tools, guarded durable store and reload bodies are production implementations.
The runner sets HOME/XDG config/state roots to its disposable directory and clears
COG_* variables before supplying synthetic fixture inputs, so host configuration
discovery cannot supply credentials. This final isolation correction `ec90830`
passes all ten affected CTests again under each of the three profiles (**30/30**).
The independent critic fixture `f129d31`, retained as `57050e7`, supplies eight
separate lifetime/error cases. No raw event is manually invented by the normal
composition fixture; older synthetic component tests remain separate coverage.

## Exact supported-profile checks

All targets compile with WERROR under GCC14.2 Release, Clang19.1 Release and
Clang19.1 RelWithDebInfo ASan/UBSan. The same selected **65/65** CTests pass for
each profile (**195/195** total). Sanitizers enable leak detection and halt on
error; the approved third-party shared dependencies are ordinary Release builds.
Build/log names are `build-observation-final-{gcc,clang,asan}` and
`/tmp/cog-observation-final-{gcc,clang,asan}-{configure,build,tests}.log`.

Every final profile uses the committed relocatable receipt and final installed
artifact, rather than the earlier development RUNPATH artifact:

```sh
cmake -S . -B BUILD -DCOG_ENABLE_WERROR=ON -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/tmp/cog-relocatable-profile/shipped/curl-build/install \
  -DCURL_DIR=/tmp/cog-relocatable-profile/shipped/curl-build/install/lib/cmake/CURL \
  -DCURL_LIBRARY=/tmp/cog-relocatable-profile/shipped/curl-build/install/lib/libcurl.so \
  -DCOG_NET_CURL_BUILD_DIR=/tmp/cog-relocatable-profile/shipped/curl-build \
  -DCOG_NET_CURL_LIBRARY=/tmp/cog-relocatable-profile/shipped/curl-build/install/lib/libcurl.so
cmake --build BUILD -j5
ctest --test-dir BUILD --output-on-failure -j4 \
  -R '^(unit-obs|unit-net.*|unit-provider.*|unit-openrouter|unit-mock-provider|integration-core-provider|integration-openrouter-https|cli-|scenario-test_(01|02|04|05|06|08|12|15|16|17|18|19|20))'
```

Clang adds `-DCMAKE_C_COMPILER=clang`; ASan uses a separate directory with
`-DCMAKE_BUILD_TYPE=RelWithDebInfo -DCOG_ENABLE_ASAN=ON` and
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` plus
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1` during CTest.
The admitted curl8.23.0/c-ares1.34.9 identities and normal-installed dependency
closure remain those in [profile evidence](REAL_CLIENT_DEPENDENCY_PROFILE_EVIDENCE.md).

All requests are synthetic and offline to disposable local endpoints; there is
no paid/live-model call. This scoped delivery does not close M, O, all38/full
runtime or release P. Independent source/test verdict and integration are still
required; accepted R18 packaging evidence remains its own gate.
