# O9 approved integration

Independent gate `4b5a102` approved exact
`f11dd5bfff863c4d059009c4f6204e351b441bd2` (last code change `3680560`).
Integration `8537bc9` merged that history into the sole `integrate-headless`
worktree, starting at accepted O4 `e8c155f`, with no conflicts.

No-ff merge: `818cc4916d548d79555982fabe31724a915dcba4`.
The approved and merged trees are both
`7c6259b24dbbcafa76363dad11ba851c3f7e84b5`.
`git diff --exit-code f11dd5b HEAD` passed before this evidence-only change.
The production diff against O4 is empty: libraries and application source/public
headers remain unchanged. No parked commits appeared in the incoming history.

## Postmerge verification

The existing checkout-local GCC build was reconfigured to Release/Werror and
built with two jobs. Existing O4 logs and its pre-O9 CMake cache were retained;
O9 writes separate logs. The admitted dependency builds are reused unchanged.

```sh
export TMPDIR="$PWD/build-o4-merge/tmp"
mapfile -t dependency_args < build-dependencies/cmake-args.txt
cmake -S . -B build-o4-merge -DCMAKE_BUILD_TYPE=Release \
  -DCOG_ENABLE_WERROR=ON -DCMAKE_C_COMPILER=gcc "${dependency_args[@]}"
cmake --build build-o4-merge --parallel 2
ctest --test-dir build-o4-merge --show-only=json-v1
ctest --test-dir build-o4-merge --output-on-failure --parallel 2
```

Full strict GCC14.2.0 Release build passed without warnings. Full CTest passed
**184/184**, with zero disabled tests, exclusions, skipped tests or failures,
in **102.44 seconds**. This includes all eleven actual canonical O9 modes,
preserved historical components, unchanged O4 native fixtures, original20/05b,
header/assertion policy checks and the existing installed embedder (0.69s).
The fixture's resource lock serializes native scenario trees while other tests
retain ordinary parallel scheduling. Actual helper flags include `-O3 -DNDEBUG`
followed by strict warnings, `-Werror` and `-UNDEBUG`.

The separate installed-proof reviewer released shared dependency installation
bookkeeping before this full run. That unmerged extension was not part of these
184 tests. Its acceptance and eventual integration remain distinct.

| Retained log | SHA-256 |
|---|---|
| `build-o4-merge/configure-o9.log` | `d3ac723b293e30439116edd55caffc0af476eb783c0c0015c1ca357446f2f8d8` |
| `build-o4-merge/build-o9.log` | `a78d767f2319c580930d2bf8c88ce7d39cca828f4143019f70c6d1fe476454b2` |
| `build-o4-merge/ctest-o9.log` | `c58b7d30b0d2fb328454941cdc4e25597e488cfd8e11469d42ecd56dde406360` |
| `build-o4-merge/test-inventory-o9.json` | `8ba95c59fc71df107ad7141e01e26007a75f6073cfdd6b137b3d31ff7214b0af` |

`build-o4-merge/o9-integration-receipt.json` records exact approved/merge/tree
identities, profile, test counts and log hashes. Incoming history and old O4
logs/cache remain available beside it. No new build or log is placed in `/tmp`.

## Scope and retained independent evidence

Independent final review separately passed full GCC184/184 (89.84s), ten
optimized readiness-case repetitions, and six exact-source Clang Release
ASan/UBSan/LSan cases (24.80s). Writer evidence retains27 scoped sanitizer passes
before the final two readiness corrections, ten corrected sanitizer repetitions,
and earlier full Clang184 scope. Those older results are not relabeled as a new
postmerge sanitizer/full-Clang run. See the [case and ownership map](REAL_CLIENT_O9_SCENARIO_EVIDENCE.md)
and ticket `4b5a102` for failures, corrected admission barriers and exact limits.
No additional TSan run is attributed to this test-only merge; no production
threading code changed. Forced-kill processes cannot provide exit-time leak
reports, and external curl/c-ares remain uninstrumented.

Writer/reviewer worktrees and failed-run/syscall diagnostics are retained for
provenance; all approved commits are now reachable from the integration branch.
O9 integration/finalize closes only `8537bc9`, `2187146` and `f63652b`. Whole O,
all38/full supported-release P, the separate installed-proof integration and any
main merge remain open. Main's user `AGENTS.md` and `build_user/` are untouched.
