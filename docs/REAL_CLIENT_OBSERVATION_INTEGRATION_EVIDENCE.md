# Independent application observation integration

Delivery `b64ee7c`, review `571c7b5`, integration `3d2ca4a`.
Independent final PASS, cycle 2 after grouped cycle-1 corrections, at
`d285aca140b105675bb44ef3e0215c69c6299690`. Production source ends at
`b6b2141`; the later checkpoint adds fixture isolation and final documentation.
Main merge `ce0c758950b117ffb79fbb2fddfdafc446752931` preserves the reviewed
history from main `0a36eb8035d7ea4f0ad70d9be22c047ccace2274`; its tree is
identical to the approved checkpoint. Existing profile, installed fixture,
relative RPATH policy and config-export fixes remain ancestors of this merge.

## Independent review

The review read the approved public contract, private fifteen-flow trace,
complete app bridge/timing/config/main/R12 deltas, normal production fixtures,
and constructor/error regressions. The first review reproduced:

- Domain clear accepted a still-owned terminal ID because active wire count
  excludes retention. Known IDs now require NOT_FOUND before clear or reuse.
- Failed reusable clear could leave a bridge target borrowing destroyed app
  obs. Final target detach now irreversibly marks the lost feed, clears every
  diagnostic borrow/target while obs lives, preserves numeric keys/foreign IDs,
  and allows consuming app destruction without leaks.
- Raw observer AGAIN could become ordinary core no-progress. Raw net/bridge
  state remains separate while app/core drive sees fatal IO.

Private header amendment `45732a6` precedes fix `9590bee`. Independent critic
tests `f129d31`, preserved as `57050e7`, cover these real ownership paths plus
callback clock/capacity and deferred sink failure. They construct actual net
IDs, cancel/release them, and exercise app/core methods without servicing those
requests. They never invent events or bypass queue ownership. Both retained and
live foreign-ID app destruction are followed by a late callback and host cleanup;
ASan/LSan finds neither a dangling target nor a retained app/obs owner. Constructor
tests exhaust all three bridge allocations under an optimized positive oracle.

The normal fixtures use real local TLS, OpenRouter decoding, guarded persistence,
edit_file execution and reload. Assertions check copied callback-time values,
true connection readiness, progress deltas/totals and terminal-once, HTTP429 versus
transport outcome, admitted retry, stall/TLS/connect errors, accepted-text cancel,
provider/tool spans, usage availability, copied generation labels and absence of
secret/body/tool-path/output/opaque markers. Original scenarios18/19 retain their
component assertions and now add production retry/tool/reload composition on the
admitted profile. HOME/XDG are disposable, so no host config/credential discovery
is needed. No paid or live-model request was made.

## Independent executable checks

Fresh ALL-TARGET WERROR builds passed under GCC14 Release, Clang19 Release and
Clang19 Release ASan/UBSan. The exact selection below passed **65/65 in each**
profile (**195/195**); leak detection and sanitizer halt were enabled.

```sh
selection='^(unit-obs|unit-net.*|unit-provider.*|unit-openrouter|unit-mock-provider|integration-core-provider|integration-openrouter-https|cli-|scenario-test_(01|02|04|05|06|08|12|15|16|17|18|19|20))'
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir BUILD -R "$selection" --output-on-failure -j4
```

The builds explicitly use the approved installed profile, not the historical
development RUNPATH artifact:

```sh
-DCMAKE_BUILD_TYPE=Release -DCOG_ENABLE_WERROR=ON
-DCMAKE_PREFIX_PATH=/tmp/cog-relocatable-profile/shipped/curl-build/install
-DCURL_DIR=/tmp/cog-relocatable-profile/shipped/curl-build/install/lib/cmake/CURL
-DCURL_LIBRARY=/tmp/cog-relocatable-profile/shipped/curl-build/install/lib/libcurl.so
-DCOG_NET_CURL_BUILD_DIR=/tmp/cog-relocatable-profile/shipped/curl-build
-DCOG_NET_CURL_LIBRARY=/tmp/cog-relocatable-profile/shipped/curl-build/install/lib/libcurl.so
```

Clang adds its explicit compiler; the separate sanitizer directory adds
`COG_ENABLE_ASAN=ON`. Logs are `/tmp/cog-appobs-final-{gcc,clang,asan}-*`.
No warning or sanitizer diagnostic occurred. The original independent eight-case
optimized runs are also retained in `/tmp/cog-appobs-critic-*-regressions.log`.

## Postmerge checks

Fresh main ALL-TARGET strict GCC Release and Clang Release ASan/UBSan builds
passed, followed by the same **65/65 each** (**130/130**). Logs are
`/tmp/cog-appobs-integration-{gcc,asan}-{configure,build,tests}.log`.

The permanent R18 driver also passed against the merged GCC build: full normal
dependency/Cog install, first relocation, independent public consumer build and
install, second relocation, actual consumer and CLI runtime. This confirms the
new private observation object links through installed exports. Its log is
`/tmp/cog-appobs-integration-installed.log`; the report is in
`build-observation-integration-gcc/installed-embedder-report.json`. Both loaded
dependency hashes match the approved profile. Ordinary dependency installation
rewrites generated install_manifest bookkeeping; no dependency build/relink or
runtime/cache/provenance mutation is implied or hidden by a read-only claim.

The preexisting untracked `build_user/` was preserved. Scoped observation tickets
may close after this evidence; implementation/review worktrees are recoverably
archived only after confirming commit retention.

This is application observability acceptance only. Deterministic single-process
joins `83b30aa`, M, N/O, all38/full-suite conformance and release P remain open.
Writer scope and commands remain in `REAL_CLIENT_OBSERVATION_EVIDENCE.md`.
