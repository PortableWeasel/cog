# O4 approved integration

Independent package gate `df377bf` approved exact
`a0a9b8bdc688060c64c5bb5649eca548005e489d`. Integration `7041c2d` merged that
history with `--no-ff` into the sole `integrate-headless` worktree, from
`d6fb056e8f6d95f2a9b42201ccbded7e646cda34`, without conflicts.

Merge commit: `bce499f832e538e5631bc191a06f181a78c7c557`.
Its tree is exactly the approved tree (`90b0fba6f269b9b7a3f2bdf574a3c9cb82cea911`);
`git diff --exit-code a0a9b8b HEAD` passed before this evidence-only commit.

Fresh post-merge strict GCC default build and full CTest suite passed:

```sh
mkdir -p build-o4-merge/tmp
export TMPDIR="$PWD/build-o4-merge/tmp"
mapfile -t dependency_args < build-dependencies/cmake-args.txt
cmake -S . -B build-o4-merge -DCOG_ENABLE_WERROR=ON \
  -DCMAKE_C_COMPILER=gcc "${dependency_args[@]}"
cmake --build build-o4-merge --parallel 2
ctest --test-dir build-o4-merge --output-on-failure --parallel 2
```

Result: **169/169 PASS**, no exclusions or skipped tests, 44.66 seconds.
Includes strict public-header smoke, actual core/IPC and installed-consumer
controls, native repeated delegation, four handoff moves, cancellation, deadline
fencing, and both actual startup-failure regressions. Retained logs:
`build-o4-merge/configure.log`, `build.log`, `ctest.log`, and CTest temporary logs.
Dependency arguments and admitted curl/c-ares artifacts remain in the ignored
`build-dependencies` tree. All new builds/logs/TMPDIR are checkout-local.

Independent pre-merge review evidence remains in ticket `df377bf` and retained
`review-o4-final` worktree: full GCC169, affected all-project ASan/UBSan/LSan13,
and TSan7 passed. Those sanitizer profiles were not rerun for this tree-identical
merge; admitted external curl/c-ares are explicitly uninstrumented. Offline
REAL loopback TLS fixtures only; no live paid calls.

Tickets `7041c2d`, `ae7f775`, and component `50fad6e` are complete after these
checks. O9 replacement scenario coverage, whole O/P gates, release packaging,
and any main-branch merge remain separate. Main/user AGENTS.md/build_user are
untouched; writer/reviewer worktrees and logs are retained for provenance.
