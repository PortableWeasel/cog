# Cog

Cog is a pre-alpha C11 LLM harness and agent framework. The libraries and CLI
support streaming/tool turns, durable sessions, explicit recovery, bounded
headless clients and local/native children. **REAL OpenRouter is the default.**
Select `--backend mock` for a credential-free offline demonstration.

Whole O is independently accepted and merged at main `c052b172`.
The supported Release profiles pass all 185 tests under GCC, Clang and
ASan/UBSan/LSan; separate TSan passes 36 selected cases, and all 28 public
headers pass 112 strict C11/C++17 combinations. These are exact admitted-profile
results, not universal platform guarantees. Supported-profile release validation
passed; ticket `77adfec` tracks acceptance. See the
[release manifest](REAL_CLIENT_RELEASE_MANIFEST.md),
[assertion map](REAL_CLIENT_RELEASE_COVERAGE.md) and [roadmap](REAL_CLIENT_WBS.md).

## Build and run an offline demonstration

Run commands from the checkout root. Keep compiler scratch and builds on disk:

```bash
mkdir -p build/tmp
export TMPDIR="$PWD/build/tmp"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCOG_ENABLE_WERROR=ON
cmake --build build --parallel 2
./build/applications/cog-cli/cog-cli --backend mock --no-store --prompt hello
./build/applications/cog-cli/cog-cli --backend mock --no-store \
  --chunks first,second --prompt hello --queue 'next turn'
```

An ordinary system curl can be unsupported for REAL transport. A mock command
does not establish the admitted HTTPS profile or full release suite.

## Admitted transport and full tests

Prepare an authenticated installable profile with the
[curl/c-ares recipe](../libraries/cog-net/cmake/relocatable/README.md).
Use checkout-local `build-dependencies/...` paths for new recipe outputs/logs;
older `/tmp` receipt paths are historical. The commands below assume the
prepared output is `build-dependencies/shipped`:

```bash
curl_build="$PWD/build-dependencies/shipped/curl-build"
cares_build="$PWD/build-dependencies/shipped/cares-build"
curl_prefix="$curl_build/install"
dependency_args=(
  "-DCMAKE_PREFIX_PATH=$curl_prefix"
  "-DCURL_DIR=$curl_prefix/lib/cmake/CURL"
  "-DCURL_LIBRARY=$curl_prefix/lib/libcurl.so"
  "-DCURL_INCLUDE_DIR=$curl_prefix/include"
  "-DCOG_NET_CURL_BUILD_DIR=$curl_build"
  "-DCOG_NET_CURL_LIBRARY=$curl_prefix/lib/libcurl.so"
  "-DCOG_TEST_CARES_BUILD_DIR=$cares_build"
)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCOG_ENABLE_WERROR=ON \
  "${dependency_args[@]}"
cmake --build build --parallel 2
ctest --test-dir build --show-only=json-v1
ctest --test-dir build --output-on-failure --parallel 2
ctest --test-dir build -L scenario --output-on-failure
```

The supported Linux/POSIX profile requires c-ares as curl's primary resolver,
`ENABLE_ARES=ON` and `ENABLE_THREADED_RESOLVER=OFF`. It uses fresh HTTP/1.1
connections, no pooling/HTTP2 and application-owned retry. Normal install and
relocation verify the admitted curl/c-ares digests and actual loader closure.
Host OpenSSL, libc, zlib/zstd and CA trust remain explicit dependencies.
Third-party dynamic memory is declared; no total dependency-memory cap exists.

Real HTTPS/OOM/installed cases are conditional on this configuration. Check the
actual inventory; an unsupported-profile selection is not the full gate.
Original 01–20 and 05b use the `scenario` label. O9 adds five fault/cancellation
modes and preserves six old substitutes under `component` labels. R01–R18 map
to concrete assertions rather than requiring R-named CTests. Native trees share
a CTest resource lock. Test assertions remain active in Release.

Clang uses a separate directory and the same dependency arguments:

```bash
cmake -S . -B build-clang -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCOG_ENABLE_WERROR=ON "${dependency_args[@]}"
cmake --build build-clang --parallel 2
ctest --test-dir build-clang --output-on-failure --parallel 2
cmake --build build --target critic-header-smoke
ctest --test-dir build -L headers --output-on-failure
```

ASan/UBSan/LSan and TSan require separate directories:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCOG_ENABLE_WERROR=ON -DCOG_ENABLE_ASAN=ON \
  "${dependency_args[@]}"
cmake --build build-asan --parallel 2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir build-asan --output-on-failure --parallel 1

cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=gcc -DCOG_ENABLE_WERROR=ON -DCOG_ENABLE_TSAN=ON \
  "${dependency_args[@]}"
cmake --build build-tsan --parallel 2
TSAN_OPTIONS=halt_on_error=1 \
ctest --test-dir build-tsan --output-on-failure --parallel 1
```

Record the actual executed selection/profile. A focused sanitizer/stress pass
does not become a full-suite result. External dependencies remain uninstrumented
unless separately rebuilt/reviewed, and killed processes cannot supply exit-time
leak reports. Dated scopes stay in their evidence records; current full-suite
acceptance carries none of the six former M exclusions.

## Use a real model

Load `OPENROUTER_API_KEY` through your own secret manager and set `COG_MODEL` to
an exact available model ID with the intended tool support. The CLI does not
choose or substitute a model. Keys are not arguments: `--credential-ref` names
their source. Precedence is defaults, bounded config file, environment, then
explicit options. This text-only example disables tools and retries:

```bash
./build/applications/cog-cli/cog-cli --model "$COG_MODEL" \
  --credential-ref env:OPENROUTER_API_KEY --no-store --tool-policy deny \
  --max-output-tokens 512 --max-retries 0 --prompt 'Say hello.'
```

For workspace work, use durable storage and an interactive policy:

```bash
mkdir -p build/demo-workspace build/demo-store
./build/applications/cog-cli/cog-cli --model "$COG_MODEL" \
  --credential-ref env:OPENROUTER_API_KEY --workspace "$PWD/build/demo-workspace" \
  --store "$PWD/build/demo-store" --tool-policy ask --interactive \
  --max-output-tokens 512 --max-tool-rounds 1 --max-retries 0 \
  --max-turn-ms 30000 --max-tool-ms 10000
```

At a tool prompt, type `allow` or `deny`. Standalone noninteractive ASK denies. Explicit
`--tool-policy allow` permits autonomous tools while retaining schema/path/budget
checks; commands are not an OS sandbox. `--no-store` is text-only. `/cancel`
cancels the active turn; `/quit` exits. SIGINT/SIGTERM drain owned work and
retain uncertain effects rather than automatically replaying them.

## Restart, fork and recover

This offline example creates and reopens a durable mock session. Choose a fresh
ID/store if `demo` already exists:

```bash
./build/applications/cog-cli/cog-cli --backend mock --store "$PWD/build/demo-store" \
  --session demo --prompt hello
./build/applications/cog-cli/cog-cli --store "$PWD/build/demo-store" --sessions
./build/applications/cog-cli/cog-cli --backend mock --store "$PWD/build/demo-store" \
  --resume demo --prompt 'next turn'
./build/applications/cog-cli/cog-cli --backend mock --store "$PWD/build/demo-store" \
  --resume demo --interactive
```

Interactive `/history`, `/tools` and `/usage` inspect the session. `/history`
supplies node IDs: select a completed replay-safe point with `/fork NODE`, or
start with `--fork demo --from-node NODE --interactive`. FORK selects a branch
in the **same durable session ID**; fresh input creates the divergent child.
It does not replay an ancestor tool. Active/unresolved work cannot be forked.

Ordinary RESUME refuses unresolved STARTED/INDETERMINATE tools. After the
operator actually fences prior writers/processes, set `SESSION` to the unresolved
ID and enter offline recovery:

```bash
./build/applications/cog-cli/cog-cli --store "$PWD/build/demo-store" \
  --recover "$SESSION" --fenced --interactive
```

Use an actually unresolved session, not the clean `demo` above. `--fenced` records
an assertion; it does not stop processes. Recovery constructs no provider/net/
workspace and needs no model/key. Inspect `/history` and `/tools`, then use
`/reconcile NODE CALL OUTCOME EVIDENCE` or
`/reconcile-index NODE ORDINAL OUTCOME EVIDENCE` (1-based original call order).
Outcomes are `succeeded`, `failed`, `cancelled` or `acknowledged`. Reconciliation
records a durable result/audit without executing tools. `/quit`, then explicitly
RESUME with fresh input when the remaining ledger is safe.

## Headless coordinator, controller and watcher

In terminal A, start an offline UI-only coordinator:

```bash
./build/applications/cog-cli/cog-cli --headless --backend mock \
  --store "$PWD/build/headless-store" --session headless-demo \
  --socket "$PWD/build/ui.sock" --prompt initial
```

In terminals B and C respectively:

```bash
./build/applications/cog-cli/cog-cli --attach "$PWD/build/ui.sock"
./build/applications/cog-cli/cog-cli --watch "$PWD/build/ui.sock"
```

The controller sends explicit input; watchers are read-only. `/quit` detaches
the controller and leaves the coordinator alive. Stop the coordinator with
SIGINT/SIGTERM in terminal A, or type `/shutdown` in the controller. In an
attached client use `/allow NONCE` or `/deny NONCE` for the currently displayed
policy prompt; standalone interactive prompts instead accept `allow`/`deny`.
Reconnect retains client state while its lease
permits; GAP requests an atomic replacement, never tool replay. Keep Unix socket
paths short. NEW refuses an existing identity; explicitly use `--resume ID` to
restart one.

Native work requires REAL rather than the mock UI demonstration:

```bash
./build/applications/cog-cli/cog-cli --headless --model "$COG_MODEL" \
  --credential-ref env:OPENROUTER_API_KEY --workspace "$PWD/build/demo-workspace" \
  --store "$PWD/build/headless-real-store" --socket "$PWD/build/real-ui.sock" \
  --worker-socket "$PWD/build/workers.sock" --workers 2 --children 3 \
  --child-requests 64 --child-tools read,search --child-policy inherit \
  --tool-policy ask --max-retries 0
```

Parent ASK decisions do not become global child ALLOW. Children are
noninteractive: inherited ASK produces identified denial before effects.
Choose policy deliberately when authorizing autonomous child tools. Children
cannot recursively delegate. Native failure retains uncertainty; it does not
automatically replay/replace admitted tasks. Migration is same-host checkpoint
transfer, not live-handle/cross-host migration. `--worker` is an owned internal
role, not a stand-alone public embedding API.

## Install and embed

With the admitted profile variables above, install normally:

```bash
prefix="$PWD/install/stage"
cmake --install "$cares_build" --prefix "$prefix"
cmake --install "$curl_build" --prefix "$prefix"
cmake --install build --prefix "$prefix"
"$prefix/bin/cog-cli" --backend mock --no-store --prompt hello
ctest --test-dir build -R '^integration-installed-embedder$' --output-on-failure
```

The external fixture checks normal install/two relocations, public headers,
relative RPATH and no loader overrides/manual staging. Its CLI-only target
checks transitive orchestrator closure. The public app API is single-process;
private `src/host_runtime.h` is not installed. Installed native lifecycle,
public REAL TLS app and MOCK UI roles are distinct proofs; remote-worker REAL
TLS is actual in-tree O9 composition. See the
[external project](../tests/external-consumer/README.md) and
[accepted receipt](REAL_CLIENT_INSTALLED_PROOF_INTEGRATION.md).

Custom stores supply a static `cog_store_vtable_t` and caller-owned
`backend_context`, alive through all guards and `cog_store_free`. Generic free
never destroys it. Fenced stores implement the complete owner/checkpoint/
recovery callback groups. Guard-wrapper allocation precedes custom normal/
recovery acquire callbacks. For a valid guard, release INVALID_STATE alone is a
nonconsuming refusal; OK, IO, AGAIN and every other result consume both backend
handle and wrapper. Clear consumed aliases and never retry them. Existing
retain-on-error backends require explicit migration. The complete public
[fenced example](../tests/external-consumer/project/fenced_store.c) and
[guard contract](../libraries/cog-core/include/cog/session_store.h) show the
tested pattern.

See the [CLI contract](../applications/cog-cli/CONTRACT.md),
[harness guide](../applications/cog-cli/HARNESS.md) and [system design](DESIGN.md).
