# Installed C11 embedder fixture (R18)

`integration-installed-embedder` is a standalone consumer, not a subdirectory
linked against Cog's build targets. It is registered on Linux when CLI, the
admitted curl profile and `COG_TEST_CARES_BUILD_DIR` are supplied. A missing
registration is not R18 release evidence.

Use a genuinely installable admitted dependency profile. The historical curl
build with a foreign `/tmp` RUNPATH is a development baseline and intentionally
fails this fixture. The profile must support normal `cmake --install --prefix`
for both dependency builds, preserve the final admitted curl bytes, use matching
relative GNUInstallDirs lib directories and provide relative loader/metadata
closure. Example producer configuration (substitute reviewed profile paths):

```sh
cmake -S . -B build-embedder -DCMAKE_BUILD_TYPE=Release -DCOG_ENABLE_WERROR=ON \
  -DCMAKE_PREFIX_PATH=<installed-dependency-prefix> \
  -DCURL_DIR=<installed-dependency-prefix>/lib/cmake/CURL \
  -DCOG_NET_CURL_BUILD_DIR=<curl-build> \
  -DCOG_NET_CURL_LIBRARY=<final-installed-curl-inside-curl-build> \
  -DCOG_TEST_CARES_BUILD_DIR=<cares-build>
cmake --build build-embedder -j4
ctest --test-dir build-embedder -R '^integration-installed-embedder$' --output-on-failure
```

For the sanitizer profile add `-DCOG_ENABLE_ASAN=ON`; the separate consumer is
compiled/linked with matching ASan/UBSan options. Its checks use an always-active
CHECK macro, including Release builds.

The driver normally installs c-ares, curl and **all** Cog install rules into a
fresh prefix, checks CMake/pkg-config/curl-config metadata, then renames that
prefix. It copies only the consumer source/CMake project into another temporary
directory. The new CMake process uses installed packages/exports only; there is
no add_subdirectory(Cog), manual header staging or private header include path.
The existing provider/net package configs supply their dependency imports;
remaining modules' raw installed target exports are explicitly loaded, with
Threads imported as their declared external requirement. No new package-config
framework is introduced.

The consumer is built and normally installed into the relocated prefix. The
whole prefix is renamed again before running the consumer and installed CLI.
ELF RUNPATH, actual loader closure, imported target locations and compile commands
are audited. No LD_LIBRARY_PATH, LD_PRELOAD or stale package-prefix environment
is used. Curl's relocated bytes must match the admitted artifact SHA; both curl
and c-ares must resolve inside the final prefix. Platform OpenSSL/libc/etc. remain
explicit system dependencies. The producer build receives a JSON audit report.

Runtime scope:

- A first-member public custom provider implements all **nine** static callbacks,
  with a declared bounded text-only profile. COMPLETE preflight accepts without
  a turn; pending tool requests are refused without acceptance. Send copies
  supported inputs, typed overflow retains the item, and poll/cancel/error,
  terminal delivery, legacy receive and disconnect are exercised. Missing
  preflight is rejected before custom connect.
- Two caller-owned custom store contexts receive their exact identities. Public
  snapshot/restore round trips use no private layout. One wrapper and its context
  are destroyed while the other remains usable; generic store_free never owns
  the caller's state, and context destructors run only after wrapper release.
- Two net contexts each own request ID1. Requests are never serviced, so there
  is no DNS/HTTP/live request. Reverse cancel/ack/release/free preserves the other
  context. Wrapped public curl global calls prove one explicit host init and one
  final cleanup, with no hidden Cog cleanup.
- All exported module graphs link. Config, IPC, obs, toolkit and mock CLI use
  supported basic lifecycle calls. The retained multi-package executable keeps its basic orchestrator
  argument-error check; the additional CLI-only executable supplies real public
  native lifecycle, route and CGW1/CGH1 checks below.

This fixture does not establish C++ standalone headers, every sparse snapshot
edge case, full O composition, all original20 plus R01-R18, or release gate P.

The whole-O extension adds a second external executable linked **only** with
`cog::cli`. Its test-only [proof boundary](project/installed_proof.h) precedes
implementation. The existing multi-package consumer stays intact. The new host
exercises installed public orchestrator/CGW1/CGH1 ownership primitives,
spawn the actual relocated `cog-cli --worker`, and prove asynchronous readiness,
shutdown and reap. A public real application uses an explicit local TLS CA.
Private `src/host_runtime.h` and execution/worker composition remain private;
they are neither copied nor installed by this fixture. Actual installed CLI
headless/attach/watch roles supply a separate binary-level proof.

The custom fenced plugin wraps a real file backend through public callbacks.
`INVALID_STATE` retains its guard; every other release result (including IO and
AGAIN) consumes it. Acquisition OOM must happen before custom normal/recovery
callbacks. These tests extend installed compatibility evidence for the approved
consuming-release contract; they introduce no new production API.


The CLI-only target imports installed exports and links no target other than
`cog::cli`; configure also requires its public interface to export
`cog::orchestrator`. Both external executables receive matching producer
ASan/UBSan or TSan flags and relative runtime paths. The driver audits actual
loader paths/digests for the new host after the second relocation as well.

Runtime assertions check a genuine native READY event against the observed
kernel PID/UID and installed executable inode, then asynchronous shutdown,
retained event ACKs, native reap and slot retirement. Its expected minimal worker
environment must exclude the synthetic credential and loader override. A fresh
Linux subreaper supervises only that external host tree; an infrastructure kill
makes a positive run fail. Separate controls cover success, exit17, SIGABRT and
timeout with escaped-session descendants, leaving no child or grandchild.

The public REAL application sends exactly two actual loopback TLS requests,
executes one real command effect, verifies its correlated TOOL result and durable
SUCCEEDED ledger, then exercises public cooperative core stop/end. The installed
CLI role check separately launches headless MOCK, watch and attach, verifies an
explicit second turn and stable session/replay identity, detaches and drains.
This installed proof does **not** claim remote-worker REAL HTTP: the installed
CLI's trust template has no local-CA input. That joined private host/worker TLS
composition remains O9's in-tree evidence. No trust bypass, private-header copy,
new public embedding surface or paid call is used.

The JSON receipt adds `public_headless_proof` (two requests, once-only effect,
owned-process disposal and installed role results). Writer validation below records the admitted GCC profile; this text does
not assert a whole-O or P pass. All producer, external compiler and process
scratch lives under the producer build tree.


Writer validation uses the accepted O4 production tree `e8c155f` plus only this
external-proof extension. Full strict GCC Release/Werror all-target build passes
in `impl-installed-proof/build-installed` with two jobs, using the existing
admitted dependency arguments from
`integrate-headless/build-dependencies/cmake-args.txt`; dependencies are not
rebuilt or copied to another development prefix. `TMPDIR` is the producer's
`build-installed/tmp`. Compiler, configuration and build logs remain there.

```sh
mkdir -p build-installed/tmp
export TMPDIR="$PWD/build-installed/tmp"
mapfile -t dependency_args < ../integrate-headless/build-dependencies/cmake-args.txt
cmake -S . -B build-installed -DCMAKE_BUILD_TYPE=Release \
  -DCOG_ENABLE_WERROR=ON -DCMAKE_C_COMPILER=gcc "${dependency_args[@]}"
cmake --build build-installed --parallel 2
ctest --test-dir build-installed \
  -R '^unit-installed-(path-audit|process-owner)$' --output-on-failure -j2
ctest --test-dir build-installed \
  -R '^integration-installed-embedder$' --output-on-failure
```

Both registered negative/control tests and the actual installed fixture pass.
`installed-embedder-report.json` records exactly two REAL loopback requests,
once-only effect, all installed role checks, native supervisor status0 with
cleanup true and **zero infrastructure kills**, plus actual relocated loader
paths. The admitted curl SHA-256 remains
`ae1a9e77488254d744c9064972b025d8eb7210e73ea900cd5f3632995bb29d18`;
c-ares remains
`b4d2377241c646dc2054e864e403f162d4b5969823bdd86ec5ff49ecf7e79f53`.
GCC/Clang strict standalone source syntax checks and Python syntax also pass.
Independent compiler/sanitizer review remains ticket `7eb47a1`; this writer
execution neither accepts whole O nor closes P.
