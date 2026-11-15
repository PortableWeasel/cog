# Cog

Cog is a pre-alpha LLM harness and agent framework written in C11. Its
explicit event loop, static dispatch tables, hierarchical sessions, mock
provider, and Unix-socket IPC are exercised by unit, integration, and
end-to-end scenario tests.

## Build and test

```bash
cmake -S . -B build -DCOG_ENABLE_WERROR=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure -j4
```

The full suite contains 61 tests: 40 unit/application/integration tests,
20 numbered end-to-end scenarios, and the scenario-05b shutdown regression.
Run only the scenario tests with:

```bash
ctest --test-dir build -L scenario --output-on-failure
```

Scenarios 01–10 (plus 05b) are under `tests/scenarios`; scenarios 11–20 are
under `applications/cog-cli/tests`. Both locations use the shared `scenario`
CTest label. The design traces are in [scenarios/](scenarios/).

Clang is supported with the same strict warning policy:

```bash
cmake -S . -B build-clang -DCMAKE_C_COMPILER=clang -DCOG_ENABLE_WERROR=ON
cmake --build build-clang -j4
ctest --test-dir build-clang --output-on-failure -j4
```

## Sanitizers

ASan/UBSan/LSan and TSan builds must remain separate.

```bash
cmake -S . -B build-asan -DCOG_ENABLE_WERROR=ON -DCOG_ENABLE_ASAN=ON
cmake --build build-asan -j4
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir build-asan --output-on-failure -j4

cmake -S . -B build-tsan -DCOG_ENABLE_WERROR=ON -DCOG_ENABLE_TSAN=ON
cmake --build build-tsan -j4
TSAN_OPTIONS=halt_on_error=1:history_size=7 \
ctest --test-dir build-tsan --output-on-failure
```

`COG_ENABLE_ASAN` enables AddressSanitizer plus UndefinedBehaviorSanitizer;
LeakSanitizer is checked with `ASAN_OPTIONS=detect_leaks=1` on supported Linux
hosts. `COG_ENABLE_TSAN` enables ThreadSanitizer and configuring both options
in one build is rejected.

To exercise the concurrency and timing-sensitive paths repeatedly:

```bash
ctest --test-dir build \
  -R '^(unit-core|unit-toolkit-parallel|integration-core-ipc|integration-core-toolkit|integration-orchestrator-ipc|integration-orchestrator-stress|scenario-test_06_parallel_tools|scenario-test_07_aux_crash|scenario-test_09_subagent_messaging|scenario-test_10_fan_out_fan_in)$' \
  --repeat until-fail:20 --output-on-failure -j2
```

The process fixtures use unique temporary paths and signal only PIDs spawned
and owned by their orchestrators.

## Run and install the CLI

The deterministic mock endpoint requires no network service or credentials:

```bash
./build/applications/cog-cli/cog-cli --prompt hello
./build/applications/cog-cli/cog-cli --prompt hello \
  --chunks first,second -q 'next turn'

cmake --install build --prefix install/stage
install/stage/bin/cog-cli --prompt hello
```

See [the CLI contract](../applications/cog-cli/CONTRACT.md) for options,
ownership, state transitions, installed targets, and tests, and
[the scenario harness guide](../applications/cog-cli/HARNESS.md) for fixture
APIs and observation points. [DESIGN.md](DESIGN.md) remains the architectural
blueprint.
