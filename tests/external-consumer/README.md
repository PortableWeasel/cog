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
  supported basic lifecycle calls. Orchestrator is symbol/argument-error closure
  only: new O async/handoff implementation remains pending.

This fixture does not establish C++ standalone headers, every sparse snapshot
edge case, O runtime behavior, all original20 plus R01-R18, or release gate P.

The whole-O extension adds a second external executable linked **only** with
`cog::cli`. Its test-only [proof boundary](project/installed_proof.h) precedes
implementation. The existing multi-package consumer stays intact. The new host
will exercise installed public orchestrator/CGW1/CGH1 ownership primitives,
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
