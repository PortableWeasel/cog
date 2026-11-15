# Optimized allocation-wrapper oracle audit

Ticket `5dd3cf7`, following independently accepted `dfabe54`; baseline main
`8c68b4a`. This is a test instrumentation correction. No production source,
public header, application implementation, runtime dependency, optimization
level or release-gate status changes.

## Controls before correction

Nine test sources serve the ten remaining active allocation-wrapped targets.
Each now starts with a direct-source positive control using its existing counter
and fault mechanism. Controls check allocation counts or the toolkit countdown,
calloc zeroed bytes, realloc retained bytes and strdup copied text where those
symbols are wrapped. Existing live-object oracles also check wrapped free.
An injected failed allocation must be counted and return NULL. A counter-only
malloc/free pair explicitly catches compiler removal of allocator calls.
Control state is reset before the original tests, whose assertions remain.

Before changing flags or state qualifiers, the first controls were built and run
against the original target settings: **all ten failed at their oracle under
both GCC Release/O3 and Clang Release/O3**. These are concrete instrumentation
failures, not evidence of unexpected production allocations. The compiler can
model standard allocators as builtins or libc leaf functions, while GNU ld later
replaces calls with wrappers that access the test's private counter/fault state.

## Target-scoped correction

Only actual wrapped symbols receive PRIVATE `-fno-builtin-<symbol>` test options.
There is no blanket `-fno-builtin`, no production option and no shared build
framework. Volatile applies only to measured test oracle state where the GCC
control still failed with the symbol flags. Session's flags-only control passed,
so its counter/fault fields retain their original ordinary types.

| Target | Symbols suppressed as builtins | Oracle state |
|---|---|---|
| test_net_cog_oom | malloc, calloc | count/fault volatile |
| test_mock_provider | malloc, calloc, realloc | count/fault volatile |
| test_mock_provider_contract | malloc, calloc, realloc | same mock source |
| test_configuration_memory | malloc, calloc, free | count/fault/live volatile |
| test_app_memory | malloc, calloc | count/fault/injection volatile |
| test_session_allocations | malloc, calloc, realloc, strdup | original ordinary state |
| test_store_fencing | malloc, calloc, realloc, strdup | allocation count/fault volatile |
| test_ipc_replay | malloc, calloc, realloc | count/fault volatile |
| test_ipc_replay_critic | malloc, calloc, free | count/fault/live volatile |
| test_toolkit_resources | malloc, calloc, realloc | countdown/deny-count volatile |

The optional test_agent wrapper configuration is documented but has no active
CMake allocator-wrapped target; it is outside this ten-target inventory.
OpenRouter's previously accepted correction remains unchanged.

## Validation and replay

Strict full target builds pass for GCC Release/O3, Clang Release/O3 and Clang
Release/O3 with ASan/UBSan. Each profile passes the final **13/13** focused set:
the ten targets above plus unit-openrouter, unit-provider-preflight-critic and
cli-session-ux. Sanitizer runs use leak detection and halt-on-error. Generated
compile options were checked in all three builds: allocator suppressions appear
only on these ten tests and the previously fixed test_openrouter; production
retains its original options and optimization level.

The bounded diagnostic [negative-control replay](../tests/check_allocation_oracle_negatives.py)
compiles temporary variants of these exact test sources and links the unchanged
optimized production archives. It does not change repository or build files:

```sh
python3 tests/check_allocation_oracle_negatives.py \
  --gcc-build build-gcc --clang-build build-clang
```

With current symbol flags retained, removing volatile from GCC oracle state
reproduces nine control failures; session remains successful as expected.
Removing the symbol flags from Clang while retaining the corrected oracle state
reproduces ten control failures. All nineteen failures stop at the named oracle
control, before the original test flow. The diagnostic requires Linux/GNU ld,
CMake Makefiles and previously built Release configurations; it is an audit
replay tool, not a new CTest framework.

No production defect was found and no paid/live request was made. Full release
validation and unrelated open P work remain separate. This writer evidence
awaits independent review before integration.

## Independent review

Critic reviewed writer checkpoint `c6ed7bb` against baseline `8c68b4a`.
The original allocation guards, constructor-fault loops, cleanup checks and
runtime no-allocation assertions remain intact. The added controls exercise
real wrapped allocations, contents, release and injected failure before
resetting their state for the original tests. Session retains ordinary state
because its flags-only control succeeds. No production source, public header,
production compile option or shared warning policy changed.

The critic found and corrected one diagnostic-script issue: GCC emits linker
dependency output as `-Wl,--dependency-file=...`, while Clang emits `-Xlinker`
followed by `--dependency-file=...`. The original relocation handled only the
latter and could overwrite GCC's existing generated `link.d`. Both spellings
now point into the diagnostic's temporary directory. This is a test-only
review correction, with no allocation-oracle behavior change.

Independent strict full Release builds completed without warnings in
`build-critic-gcc`, `build-critic-clang` and `build-critic-asan` (Clang with
ASan/UBSan and leak detection). All three passed the same 13 scoped tests.
An initial sanitizer CTest invocation preceded completion of that build and
reported seven missing executables; it is not counted as test evidence. The
complete-build invocation passed 13/13 with no sanitizer report.

The complete twenty-variant negative replay against the independent GCC and
Clang builds reproduced nine GCC oracle failures plus session's expected
success, and ten Clang oracle failures. Every expected failure named
`allocation_oracle_control`. SHA-256 contents and nanosecond modification times
of all 3,457 existing files in those two builds matched before and after the
replay, with no added/deleted build files and no remaining new temporary
directory. Thus both actual linker spellings were exercised without changing
existing build artifacts. Generated flags in all three builds were also
checked: only the ten scoped test targets and previously corrected
`test_openrouter` contain allocator builtin suppressions.

Review verdict: **PASS, cycle 1**, including the narrow script correction.
This approves the scoped test instrumentation change; integration and the
parent P release gate remain separate. No paid or live request was made.

## Main integration

Approved `d53f188` was merged without conflicts into main `8c68b4a` by
`56697197ba14b3a6542cac53fec7d5a7297e9913` (no fast-forward). The merged
tree exactly matches the approved tree. Existing `build_user/` and the
independent header-design worktrees were preserved.

Fresh main `build-review-allocation-oracles` completed a full strict Clang
Release build without warnings and passed all 13 scoped tests. Postmerge
negative controls exercised session and IPC replay with both profiles: GCC
nonvolatile session succeeded and IPC failed at the oracle; Clang without the
symbol flags failed at both oracles. This checks the deliberate session
exception and both linker dependency-output spellings without repeating the
unchanged twenty-case review.

SHA-256 contents and nanosecond modification times of all 3,460 existing files
in the main Clang build and prior independent GCC build matched before/after
these four replays, with no added/deleted files or temporary-directory remnant.
The sorted artifact manifest's SHA-256 was
`3025a03a1e03e5071b756b13380367071b391aacf886de352444bb7efd8c7af9`.
Integration closes only `5dd3cf7`; parent P full validation remains open.
