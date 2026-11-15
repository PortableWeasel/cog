# L independent review and main integration

Tickets: L `ff2dbc9`, independent review `e509146`, integration `631d004`,
allocation-oracle repair `dfabe54`. Reviewed application checkpoint
`cb3ad50bfd158054941f6d39adfb951510df37a5`; final approved checkpoint
`4a53bd572514cbea1dad5817b6ea553963f8c88c`. Main merge
`cf2572d8182777799fad646143d5029c17bef7cd` preserves the reviewed history
with a two-parent merge from `594128969243573ed17a788f530c3855680b254f`.
The merge tree equals the final approved checkpoint exactly.

## Independent review

PASS, L review cycle 2. The initial source review found three terminal defects:
unknown-usage output did not check its flush; recovery polling could spin on
invalid descriptors; full command bytes were not validated before C-string
dispatch. Writer fix `0eec4a9` and its permanent regressions were reviewed and
executed in all three profiles below.

The final review covered exact checkpoint restoration, owner epoch/incarnation
and admission, explicit fenced adoption/recovery, no automatic effect replay,
failed recovery-save freeze while ownership stays held, current-policy admission
of restored proposals only after provider preflight, partial-turn authorization,
same-session fork isolation, metadata-only construction, output/EOF/signals, and
installed CLI restart/recovery. `/reconcile-index` uses the immutable original
assistant call order; literal IDs such as `@2` stay literal, and whitespace IDs
reach reconciliation without rewriting their stored bytes. The three-pass
identity trace is `docs/scenarios/L01-terminal-reconcile-identity.md`.

Independent strict all-target builds passed with GCC 14 Release, Clang 19
Release, and Clang 19 Debug ASan/UBSan. The following exact selection passed
38/38 in each profile, with leak checking and sanitizer halt enabled:

```sh
selection='^(cli-(options|configuration|configuration-memory|app-loop|app-stop|app-memory|app-save|app-limits|app-codec-critic|host-signal|app-https|session-ux|installed-session-ux|session-recovery-faults)|unit-(session|session-codec|session-allocations|session-store|store-adversarial|store-fencing|core|core-service|provider|provider-critic|provider-preflight-critic|openrouter|mock-provider|toolkit-async|workspace|toolkit-fencing|toolkit-review-faults)|integration-openrouter-https|build-test-.*)$'
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir BUILD -R "$selection" --output-on-failure -j4
```

The previously failing OpenRouter test, provider-preflight critic test, and
three new L tests also passed three repetitions per profile (45 executions).
All 28 public headers compiled standalone under strict GCC and Clang warnings.
Additional actual offline CLI probes verified 1,024 IDs produce 1,024 sorted
lines, 1,025 IDs refuse with LIMIT and empty stdout, and an invalid UTF-8 store
ID refuses with PROTOCOL and empty stdout. These probes used temporary stores,
synthetic home directories and no provider credentials.

## Allocation-oracle repair

The expanded Clang Release selection initially failed OpenRouter's unchanged
allocation-counter assertion. LLVM IR and diagnostic counters identified a
fixture allocation before preflight whose counter baseline Clang reused across
the linker-wrapped allocator. Preflight itself made no network or allocator
call; its forbid-allocation assertion never fired. Ticket `dfabe54` records the
diagnosis rather than treating the failure as a passing test.

The approved two-file repair applies allocator-specific no-builtin options only
to `test_openrouter`, observes wrapper counter/fault writes with volatile test
fields, and adds a positive oracle for malloc/calloc/realloc, retained realloc
bytes, calloc zeroes, and injected failure. Production flags and the original
no-network/no-allocation assertions remain unchanged. Independent optimized
negative controls removed only volatile under GCC O3 or only the three
no-builtin options under Clang O3; both aborted in the new oracle, while the
committed fixture passed. Other wrapped allocation fixtures remain the separate
P audit `5dd3cf7`.

## Main verification and limits

Fresh main all-target strict GCC Release and Clang Debug ASan/UBSan builds passed
at the merge, followed by the same 38/38 selection in each. Sanitizer execution
used the environment shown above. The builds used the maintained local c-ares
curl profile through these explicit CMake settings:

```sh
-DCOG_ENABLE_WERROR=ON
-DCOG_NET_CURL_BUILD_DIR=/tmp/cog-net-maintained/build
-DCOG_NET_CURL_LIBRARY=/tmp/cog-net-maintained/build/lib/libcurl.so
-DCURL_LIBRARY=/tmp/cog-net-maintained/build/lib/libcurl.so
-DCURL_INCLUDE_DIR=/tmp/cog-net-maintained/curl-8.23.0/include
```

Independent logs were preserved as `/tmp/cog-l-independent-{gcc,clang,asan}-*`;
postmerge logs are `/tmp/cog-l-integration-{gcc,asan}-{configure,build,tests}.log`.
The original writer profile evidence remains `docs/REAL_CLIENT_L_EVIDENCE.md`.
The preexisting untracked `build_user/` was preserved.

This closes scoped L delivery and its allocation-oracle integration repair.
No paid provider calls were made. R12 generation/reload work (`dffc59e`,
`af6956a`), M, N, O, and the full P conformance/sanitizer gates remain open.
These focused results do not claim all 20 original scenarios or R01–R18 have
passed their final runtime gate.
